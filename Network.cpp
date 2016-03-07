/****************************************************************************************************

 RepRapFirmware - Network: RepRapPro Ormerod with Arduino Due controller

  2014-04-05 Created from portions taken out of Platform.cpp by dc42
  2014-04-07 Added portions of httpd.c. These portions are subject to the following copyright notice:

 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 * (end httpd.c copyright notice)

 ****************************************************************************************************/

#include "RepRapFirmware.h"

extern "C"
{
#include "ethernet_sam.h"

#include "lwipopts.h"
#ifdef LWIP_STATS
#include "lwip/src/include/lwip/stats.h"
#endif
#include "lwip/src/include/lwip/tcp.h"
#include "lwip/src/include/lwip/tcp_impl.h"

#include "contrib/apps/netbios/netbios.h"
#include "contrib/apps/mdns/mdns_responder.h"
}

static volatile bool lwipLocked = false;
static volatile bool hasLink = false;

static tcp_pcb *http_pcb = nullptr;
static tcp_pcb *ftp_main_pcb = nullptr;
static tcp_pcb *ftp_pasv_pcb = nullptr;
static tcp_pcb *telnet_pcb = nullptr;


static struct mdns_service mdns_services[] = {
	{
		.name = "\x05_echo\x04_tcp\x05local",
		.port = 7,
	},
	{
		.name = "\x05_http\x04_tcp\x05local",
		.port = DEFAULT_HTTP_PORT,
	},
	{
		.name = "\x04_ftp\x04_tcp\x05local",
		.port = FTP_PORT
	},
	{
		.name = "\x07_telnet\x04_tcp\x05local",
		.port = TELNET_PORT
	}
};

const size_t MDNS_HTTP_SERVICE_INDEX = 1;	// Index of the mDNS HTTP service above

static const char *mdns_txt_records[] = {
	"product=" NAME,
	"version=" VERSION,
	NULL
};


static bool closingDataPort = false;

static ConnectionState *sendingConnection = nullptr;
static uint32_t sendingWindow32[(TCP_WND + 3)/4];						// should be 32-bit aligned for efficiency
static inline char* sendingWindow() { return reinterpret_cast<char*>(sendingWindow32); }
static uint16_t sendingWindowSize, sentDataOutstanding;
static uint8_t sendingRetries;

static uint16_t httpPort = DEFAULT_HTTP_PORT;


/*-----------------------------------------------------------------------------------*/

extern "C"
{
// Lock functions for LWIP (LWIP generally isn't thread-safe)

bool LockLWIP()
{
	if (lwipLocked)
		return false;

	lwipLocked = true;
	return true;
}

void UnlockLWIP()
{
	lwipLocked = false;
}

// Callback functions for the EMAC driver

// Callback to report when the link has gone up or down
static void ethernet_status_callback(struct netif *netif)
{
	hasLink = netif_is_up(netif);
	if (hasLink)
	{
		char ip[16];
		ipaddr_ntoa_r(&(netif->ip_addr), ip, sizeof(ip));
		reprap.GetPlatform()->MessageF(HOST_MESSAGE, "Network up, IP=%s\n", ip);
	}
	else
	{
		reprap.GetPlatform()->Message(HOST_MESSAGE, "Network down\n");
	}
}

// Called from ISR
static void ethernet_rx_callback(uint32_t ul_status)
{
	// Because the LWIP stack can become corrupted if we work with it in parallel,
	// we may have to wait for the next Spin() call to read the next packet.
	if (LockLWIP())
	{
		ethernet_task();
		UnlockLWIP();
	}
	else
	{
		reprap.GetNetwork()->ReadPacket();
		ethernet_set_rx_callback(nullptr);
	}
}


// Callback functions for LWIP (may be called from ISR)

static void conn_err(void *arg, err_t err)
{
	// Report the error to the monitor
	reprap.GetPlatform()->MessageF(HOST_MESSAGE, "Network: Connection error, code %d\n", err);

	ConnectionState *cs = (ConnectionState*)arg;
	if (cs != nullptr)
	{
		if (reprap.Debug(moduleNetwork))
		{
			reprap.GetPlatform()->Message(HOST_MESSAGE, "Network: This connection has a valid CS\n");
		}

		// Tell the higher levels about the error
		reprap.GetNetwork()->ConnectionClosed(cs, false);
	}
}

/*-----------------------------------------------------------------------------------*/

static err_t conn_poll(void *arg, tcp_pcb *pcb)
{
	ConnectionState *cs = (ConnectionState*)arg;
	if (cs == sendingConnection)
	{
		// Data could not be sent in time, check if the connection has to be timed out
		sendingRetries++;
		if (sendingRetries == TCP_MAX_SEND_RETRIES)
		{
			reprap.GetPlatform()->MessageF(HOST_MESSAGE, "Network: Poll couldn't send data after %d retries!\n", TCP_MAX_SEND_RETRIES);
			tcp_abort(pcb);
			return ERR_ABRT;
		}

		// Do we have enough space left for sending? Third-party apps may be sending data as well, so check this first
		if (tcp_sndbuf(pcb) < TCP_WND)
		{
			if (reprap.Debug(moduleNetwork))
			{
				reprap.GetPlatform()->Message(HOST_MESSAGE, "Network: Could not send data because not enough sending space is available\n");
			}
			return ERR_MEM;
		}

		// Try to send the remaining data once again
		err_t err = tcp_write(pcb, sendingWindow() + (sendingWindowSize - sentDataOutstanding), sentDataOutstanding, 0);
		if (err == ERR_OK)
		{
			err = tcp_output(pcb);
		}

		if (ERR_IS_FATAL(err))
		{
			reprap.GetPlatform()->MessageF(HOST_MESSAGE, "Network: Failed to write data in conn_poll (code %d)\n", err);
			tcp_abort(pcb);
			return ERR_ABRT;
		}
	}
	else
	{
		reprap.GetPlatform()->Message(HOST_MESSAGE, "Network: Mismatched pcb in conn_poll!\n");
	}
	return ERR_OK;
}

/*-----------------------------------------------------------------------------------*/

static err_t conn_sent(void *arg, tcp_pcb *pcb, u16_t len)
{
	ConnectionState *cs = (ConnectionState*)arg;
	if (cs == sendingConnection)
	{
		if (sentDataOutstanding > len)
		{
			sentDataOutstanding -= len;
		}
		else
		{
			tcp_poll(pcb, nullptr, TCP_WRITE_TIMEOUT / TCP_SLOW_INTERVAL / TCP_MAX_SEND_RETRIES);
			sendingConnection = nullptr;
		}
	}
	else
	{
		reprap.GetPlatform()->Message(HOST_MESSAGE, "Network: Mismatched pcb in conn_sent!\n");
	}
	return ERR_OK;
}

/*-----------------------------------------------------------------------------------*/

static err_t conn_recv(void *arg, tcp_pcb *pcb, pbuf *p, err_t err)
{
	ConnectionState *cs = (ConnectionState*)arg;
	if (err == ERR_OK && cs != nullptr)
	{
		if (cs->pcb != pcb)
		{
			reprap.GetPlatform()->Message(HOST_MESSAGE, "Network: Mismatched pcb in conn_recv!\n");
			tcp_abort(pcb);
			return ERR_ABRT;
		}

		bool processingOk = true;
		if (p != nullptr)
		{
			// Tell higher levels that we are receiving data
			processingOk = reprap.GetNetwork()->ReceiveInput(p, cs);
		}
		else if (cs->persistConnection)
		{
			// Tell higher levels that a connection has been closed
			processingOk = reprap.GetNetwork()->ConnectionClosedGracefully(cs);
		}

		if (!processingOk)
		{
			if (p != nullptr)
			{
				pbuf_free(p);
			}
			tcp_abort(pcb);
			return ERR_ABRT;
		}
	}

	return ERR_OK;
}

/*-----------------------------------------------------------------------------------*/

static err_t conn_accept(void *arg, tcp_pcb *pcb, err_t err)
{
	LWIP_UNUSED_ARG(arg);
	LWIP_UNUSED_ARG(err);

	/* Allocate a new ConnectionState for this connection */
	ConnectionState *cs = reprap.GetNetwork()->ConnectionAccepted(pcb);
	if (cs == nullptr)
	{
		tcp_abort(pcb);
		return ERR_ABRT;
	}

	/* Keep the listening PCBs running */
	switch (pcb->local_port)		// tell LWIP to accept further connections on the listening PCB
	{
		case FTP_PORT:		// FTP
			tcp_accepted(ftp_main_pcb);
			break;

		case TELNET_PORT:	// Telnet
			tcp_accepted(telnet_pcb);
			break;

		default:			// HTTP and FTP data
			tcp_accepted((pcb->local_port == httpPort) ? http_pcb : ftp_pasv_pcb);
			break;
	}
	tcp_arg(pcb, cs);			// tell LWIP that this is the structure we wish to be passed for our callbacks
	tcp_recv(pcb, conn_recv);	// tell LWIP that we wish to be informed of incoming data by a willcall to the conn_recv() function
	tcp_err(pcb, conn_err);

	return ERR_OK;
}

} // end extern "C"

/*-----------------------------------------------------------------------------------*/

void httpd_init()
{
	tcp_pcb* pcb = tcp_new();
	tcp_bind(pcb, IP_ADDR_ANY, httpPort);
	http_pcb = tcp_listen(pcb);
	tcp_accept(http_pcb, conn_accept);
}

void ftpd_init()
{
	tcp_pcb* pcb = tcp_new();
	tcp_bind(pcb, IP_ADDR_ANY, FTP_PORT);
	ftp_main_pcb = tcp_listen(pcb);
	tcp_accept(ftp_main_pcb, conn_accept);
}

void telnetd_init()
{
	tcp_pcb* pcb = tcp_new();
	tcp_bind(pcb, IP_ADDR_ANY, TELNET_PORT);
	telnet_pcb = tcp_listen(pcb);
	tcp_accept(telnet_pcb, conn_accept);
}

//***************************************************************************************************

// Network/Ethernet class

Network::Network(Platform* p)
	: platform(p), freeTransactions(nullptr), readyTransactions(nullptr), writingTransactions(nullptr),
	state(NetworkInactive), isEnabled(true), readingData(false),
	dataCs(nullptr), ftpCs(nullptr), telnetCs(nullptr), freeConnections(nullptr)
{
	for (size_t i = 0; i < NETWORK_TRANSACTION_COUNT; i++)
	{
		freeTransactions = new NetworkTransaction(freeTransactions);
	}

	for (size_t i = 0; i < MEMP_NUM_TCP_PCB; i++)
	{
		ConnectionState *cs = new ConnectionState;
		cs->next = freeConnections;
		freeConnections = cs;
	}

	strcpy(hostname, HOSTNAME);
}

void Network::AppendTransaction(NetworkTransaction* volatile* list, NetworkTransaction *r)
{
	r->next = nullptr;
	while (*list != nullptr)
	{
		list = &((*list)->next);
	}
	*list = r;
}

void Network::PrependTransaction(NetworkTransaction* volatile* list, NetworkTransaction *r)
{
	r->next = *list;
	*list = r;
}

void Network::Init()
{
	longWait = platform->Time();
	state = NetworkPreInitializing;
}

void Network::Spin()
{
	// Basically we can't do anything if we can't interact with LWIP
	if (!isEnabled || !LockLWIP())
	{
		platform->ClassReport(longWait);
		return;
	}

	if (state == NetworkActive && hasLink)
	{
		// See if we can read any packets
		if (readingData)
		{
			ethernet_task();
			readingData = false;
			ethernet_set_rx_callback(&ethernet_rx_callback);
		}

		// See if we can send anything
		NetworkTransaction *r = writingTransactions;
		if (r != nullptr && sendingConnection == nullptr)
		{
			if (r->next != nullptr)
			{
				// Data is supposed to be sent to another connection and the last packet has
				// been acknowledged. Rotate the sending transactions so every client is
				// served even while big files are being sent.
				NetworkTransaction *rn = r->next;
				writingTransactions = rn;
				AppendTransaction(&writingTransactions, r);
				r = rn;
			}

			if (r->Send())
			{
				// We're done, free up this transaction
				ConnectionState *cs = r->cs;
				NetworkTransaction *rn = r->nextWrite;
				writingTransactions = r->next;
				AppendTransaction(&freeTransactions, r);

				// If there is more data to write on this connection, do it next time
				if (cs != nullptr)
				{
					cs->sendingTransaction = rn;
				}
				if (rn != nullptr)
				{
					PrependTransaction(&writingTransactions, rn);
				}
			}
		}
	}
	else if (state == NetworkPostInitializing && ethernet_establish_link())
	{
		start_ethernet(platform->IPAddress(), platform->NetMask(), platform->GateWay(), &ethernet_status_callback);
		ethernet_set_rx_callback(&ethernet_rx_callback);

		httpd_init();
		ftpd_init();
		telnetd_init();

		netbios_init();
		mdns_responder_init(mdns_services, ARRAY_SIZE(mdns_services), mdns_txt_records);
		mdns_announce(); // NB: Wireless bridges like the TP-Link WR702N don't route incoming IGMP packets, so send an mDNS announcement as a fall-back option

		state = isEnabled ? NetworkActive : NetworkInactive;
	}

	UnlockLWIP();
	platform->ClassReport(longWait);
}

void Network::Interrupt()
{
	if (isEnabled && state == NetworkActive && LockLWIP())
	{
		ethernet_timers_update();
		UnlockLWIP();
	}
}

void Network::Diagnostics()
{
	platform->Message(GENERIC_MESSAGE, "Network Diagnostics:\n");

	size_t numFreeConnections = 0;
	ConnectionState *freeConn = freeConnections;
	while (freeConn != nullptr)
	{
		numFreeConnections++;
		freeConn = freeConn->next;
	}
	platform->MessageF(GENERIC_MESSAGE, "Free connections: %d of %d\n", numFreeConnections, MEMP_NUM_TCP_PCB);

	size_t numFreeTransactions = 0;
	NetworkTransaction *freeTrans = freeTransactions;
	while (freeTrans != nullptr)
	{
		numFreeTransactions++;
		freeTrans = freeTrans->next;
	}
	platform->MessageF(GENERIC_MESSAGE, "Free transactions: %d of %d\n", numFreeTransactions, NETWORK_TRANSACTION_COUNT);

#if LWIP_STATS
	// Normally we should NOT try to display LWIP stats here, because it uses debugPrintf(), which will hang the system is no USB cable is connected.
	if (reprap.Debug(moduleNetwork))
	{
		stats_display();
	}
#endif
}

void Network::Enable()
{
	if (state == NetworkPreInitializing)
	{
		// We must call this one only once, otherwise we risk a firmware crash
		init_ethernet(platform->MACAddress(), hostname);
		state = NetworkPostInitializing;
	}

	if (!isEnabled)
	{
		readingData = true;
		isEnabled = true;
		// EMAC RX callback will be reset on next Spin calls

		if (state == NetworkInactive)
		{
			state = NetworkActive;
		}
	}
}

void Network::Disable()
{
	if (isEnabled)
	{
		readingData = false;
		ethernet_set_rx_callback(nullptr);
		if (state == NetworkActive)
		{
			state = NetworkInactive;
		}
		isEnabled = false;
	}
}

bool Network::IsEnabled() const
{
	return isEnabled;
}

uint16_t Network::GetHttpPort() const
{
	return httpPort;
}

void Network::SetHttpPort(uint16_t port)
{
	if (state == NetworkActive && port != httpPort)
	{
		// Close old HTTP port
		tcp_close(http_pcb);

		// Create a new one for the new port
		tcp_pcb* pcb = tcp_new();
		tcp_bind(pcb, IP_ADDR_ANY, port);
		http_pcb = tcp_listen(pcb);
		tcp_accept(http_pcb, conn_accept);

		// Update mDNS services
		mdns_services[MDNS_HTTP_SERVICE_INDEX].port = port;
		mdns_announce();
	}
	httpPort = port;
}

// This is called when a connection is being established and returns an initialised ConnectionState instance.
ConnectionState *Network::ConnectionAccepted(tcp_pcb *pcb)
{
	ConnectionState *cs = freeConnections;
	if (cs == nullptr)
	{
		platform->Message(HOST_MESSAGE, "Network::ConnectionAccepted() - no free ConnectionStates!\n");
		return nullptr;
	}

	NetworkTransaction* r = freeTransactions;
	if (r == nullptr)
	{
		platform->Message(HOST_MESSAGE, "Network::ConnectionAccepted() - no free transactions!\n");
		return nullptr;
	}

	freeConnections = cs->next;
	cs->Init(pcb);

	r->Set(nullptr, cs, connected);
	freeTransactions = r->next;
	AppendTransaction(&readyTransactions, r);

	return cs;
}

// This is called when a connection is being closed or has gone down unexpectedly
void Network::ConnectionClosed(ConnectionState* cs, bool closeConnection)
{
	// Make sure these connections are not reused. Remove all references to it
	if (cs == dataCs)
	{
		dataCs = nullptr;
	}
	if (cs == ftpCs)
	{
		ftpCs = nullptr;
	}
	if (cs == telnetCs)
	{
		telnetCs = nullptr;
	}
	if (cs == sendingConnection)
	{
		sendingConnection = nullptr;
	}

	// Close it if requested
	tcp_pcb *pcb = cs->pcb;
	if (pcb != nullptr && closeConnection)
	{
		tcp_arg(pcb, nullptr);
		tcp_sent(pcb, nullptr);
		tcp_recv(pcb, nullptr);
		tcp_poll(pcb, nullptr, TCP_WRITE_TIMEOUT / TCP_SLOW_INTERVAL / TCP_MAX_SEND_RETRIES);
		tcp_close(pcb);
	}
	cs->pcb = nullptr;

	// Inform the Webserver that we are about to remove an existing connection
	reprap.GetWebserver()->ConnectionLost(cs);

	// cs points to a connection state block that the caller is about to release, so we need to stop referring to it.
	// There may be one NetworkTransaction in the writing or closing list referring to it, and possibly more than one in the ready list.
	for (NetworkTransaction* r = readyTransactions; r != nullptr; r = r->next)
	{
		if (r->cs == cs)
		{
			r->SetConnectionLost();
		}
	}

	if (cs->sendingTransaction != nullptr)
	{
		cs->sendingTransaction->SetConnectionLost();
		cs->sendingTransaction = nullptr;
	}

	cs->next = freeConnections;
	freeConnections = cs;
}

// This enqueues a new transaction to indicate a graceful reset. Do this to keep the time line of incoming transactions valid
bool Network::ConnectionClosedGracefully(ConnectionState *cs)
{
	NetworkTransaction* r = freeTransactions;
	if (r == nullptr)
	{
		platform->Message(HOST_MESSAGE, "Network::ConnectionClosedGracefully() - no free transactions!\n");
		return false;
	}

	freeTransactions = r->next;
	r->Set(nullptr, cs, disconnected);
	AppendTransaction(&readyTransactions, r);
	return true;
}

bool Network::Lock()
{
	return LockLWIP();
}

void Network::Unlock()
{
	UnlockLWIP();
}

bool Network::InLwip() const
{
	return lwipLocked;
}

void Network::ReadPacket()
{
	readingData = true;
}

bool Network::ReceiveInput(pbuf *pb, ConnectionState* cs)
{
	NetworkTransaction* r = freeTransactions;
	if (r == nullptr)
	{
		platform->Message(HOST_MESSAGE, "Network::ReceiveInput() - no free transactions!\n");
		return false;
	}

	freeTransactions = r->next;
	r->Set(pb, cs, dataReceiving);

	AppendTransaction(&readyTransactions, r);
//	debugPrintf("Network - input received\n");
	return true;
}

// This is called by the web server to get a new received packet.
// If the connection parameter is nullptr, we just return the request at the head of the ready list.
// Otherwise, we are only interested in packets received from the specified connection. If we find one then
// we move it to the head of the ready list, so that a subsequent call with a null connection parameter
// will return the same one.
NetworkTransaction *Network::GetTransaction(const ConnectionState *cs)
{
	// See if there is any transaction at all
	NetworkTransaction *rs = readyTransactions;
	if (rs == nullptr)
	{
		return nullptr;
	}

	// If we're waiting for a new connection on a data port, see if there is a matching transaction available
	if (cs == nullptr && rs->waitingForDataConnection)
	{
		for (NetworkTransaction *rsNext = rs->next; rsNext != nullptr; rsNext = rs->next)
		{
			if (rsNext->status == connected && rsNext->GetLocalPort() > 1023)
			{
				rs->next = rsNext->next;		// remove rsNext from the list
				rsNext->next = readyTransactions;
				readyTransactions = rsNext;
				return rsNext;
			}

			rs = rsNext;
		}

		return readyTransactions;	// nothing found, process this transaction once again
	}

	// See if the first one is the transaction we're looking for
	if (cs == nullptr || rs->cs == cs)
	{
		return rs;
	}

	// There is at least one ready transaction, but it's not on the connection we are looking for
	for (NetworkTransaction *rsNext = rs->next; rsNext != nullptr; rsNext = rs->next)
	{
		if (rsNext->cs == cs)
		{
			rs->next = rsNext->next;		// remove rsNext from the list
			rsNext->next = readyTransactions;
			readyTransactions = rsNext;
			return rsNext;
		}

		rs = rsNext;
	}

	return nullptr;
}

// The current NetworkTransaction must be processed again,
// e.g. because we're still waiting for another data connection.
void Network::WaitForDataConection()
{
	NetworkTransaction *r = readyTransactions;
	r->waitingForDataConnection = true;
	r->inputPointer = 0; // behave as if this request hasn't been processed yet
}

const uint8_t *Network::IPAddress() const
{
	return ethernet_get_ipaddress();
}

void Network::SetIPAddress(const uint8_t ipAddress[], const uint8_t netmask[], const uint8_t gateway[])
{
	if (state == NetworkActive)
	{
		// This performs IP changes on-the-fly
		ethernet_set_configuration(ipAddress, netmask, gateway);

		// Announce mDNS services again
		mdns_announce();
	}
}

void Network::OpenDataPort(uint16_t port)
{
	closingDataPort = false;
	tcp_pcb* pcb = tcp_new();
	tcp_bind(pcb, IP_ADDR_ANY, port);
	ftp_pasv_pcb = tcp_listen(pcb);
	tcp_accept(ftp_pasv_pcb, conn_accept);
}

uint16_t Network::GetDataPort() const
{
	return (closingDataPort || (ftp_pasv_pcb == nullptr) ? 0 : ftp_pasv_pcb->local_port);
}

// Close FTP data port and purge associated PCB
void Network::CloseDataPort()
{
	// See if it's already being closed
	if (closingDataPort)
	{
		return;
	}
	closingDataPort = true;

	// Close remote connection of our data port or do it as soon as the current transaction has finished
	if (dataCs != nullptr && dataCs->pcb != nullptr)
	{
		NetworkTransaction *mySendingTransaction = dataCs->sendingTransaction;
		if (mySendingTransaction != nullptr)
		{
			mySendingTransaction->Close();
			return;
		}
	}

	// We can close it now, so do it here
	if (ftp_pasv_pcb != nullptr)
	{
		tcp_accept(ftp_pasv_pcb, nullptr);
		tcp_close(ftp_pasv_pcb);
		ftp_pasv_pcb = nullptr;
	}
	closingDataPort = false;
}

// These methods keep track of our connections in case we need to send to one of them
void Network::SaveDataConnection()
{
	dataCs = readyTransactions->cs;
}

void Network::SaveFTPConnection()
{
	ftpCs = readyTransactions->cs;
}

void Network::SaveTelnetConnection()
{
	telnetCs = readyTransactions->cs;
}

// Check if there are enough resources left to allocate another NetworkTransaction for sending
bool Network::CanAcquireTransaction()
{
	return (freeTransactions != nullptr);
}

bool Network::AcquireFTPTransaction()
{
	return AcquireTransaction(ftpCs);
}

bool Network::AcquireDataTransaction()
{
	return AcquireTransaction(dataCs);
}

bool Network::AcquireTelnetTransaction()
{
	return AcquireTransaction(telnetCs);
}

// Retrieves the NetworkTransaction of a sending connection to which data can be appended to,
// or prepares a released NetworkTransaction, which can easily be sent via Commit().
bool Network::AcquireTransaction(ConnectionState *cs)
{
	// Make sure we have a valid connection
	if (cs == nullptr)
	{
		return false;
	}

	// If our current transaction already belongs to cs and can be used, don't look for another one
	NetworkTransaction *currentTransaction = readyTransactions;
	if (currentTransaction != nullptr && currentTransaction->GetConnection() == cs && currentTransaction->fileBeingSent == nullptr)
	{
		return true;
	}

	// See if we're already writing on this connection
	NetworkTransaction *firstTransaction = cs->sendingTransaction;
	NetworkTransaction *lastTransaction = firstTransaction;
	if (lastTransaction != nullptr)
	{
		while (lastTransaction->nextWrite != nullptr)
		{
			lastTransaction = lastTransaction->nextWrite;
		}
	}

	// Then check if this transaction is valid and safe to use
	NetworkTransaction *transactionToUse;
	if (firstTransaction != nullptr && firstTransaction != lastTransaction && lastTransaction->fileBeingSent == nullptr)
	{
		transactionToUse = lastTransaction;
	}
	// We cannot use it, so try to allocate a free one
	else
	{
		transactionToUse = freeTransactions;
		if (transactionToUse == nullptr)
		{
			platform->Message(HOST_MESSAGE, "Network: Could not acquire free transaction!\n");
			return false;
		}
		freeTransactions = transactionToUse->next;
		transactionToUse->Set(nullptr, cs, dataReceiving); // set it to dataReceiving as we expect a response
	}

	// Replace the first entry of readyTransactions with our new transaction, so it can be used by Commit().
	PrependTransaction(&readyTransactions, transactionToUse);
	return true;
}

// Set the DHCP hostname. Removes all whitespaces and converts the name to lower-case.
void Network::SetHostname(const char *name)
{
	size_t i = 0;
	while (*name && i < ARRAY_UPB(hostname))
	{
		char c = *name++;
		if (c >= 'A' && c <= 'Z')
		{
			c += 'a' - 'A';
		}

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-') || (c == '_'))
		{
			hostname[i++] = c;
		}
	}

	if (i > 0)
	{
		hostname[i] = 0;
	}
	else
	{
		// Don't allow empty hostnames
		strcpy(hostname, HOSTNAME);
	}

	if (state == NetworkActive)
	{
		mdns_update_hostname();
	}
}

//***************************************************************************************************

// ConnectionState class

void ConnectionState::Init(tcp_pcb *p)
{
	pcb = p;
	localPort = p->local_port;
	remoteIPAddress = p->remote_ip.addr;
	remotePort = p->remote_port;
	next = nullptr;
	sendingTransaction = nullptr;
	persistConnection = true;
}

//***************************************************************************************************

// NetworkTransaction class

NetworkTransaction::NetworkTransaction(NetworkTransaction *n) : next(n)
{
	sendStack = new OutputStack();
}

void NetworkTransaction::Set(pbuf *p, ConnectionState *c, TransactionStatus s)
{
	cs = c;
	pb = readingPb = p;
	bufferLength = (p == nullptr) ? 0 : pb->tot_len;
	status = s;
	inputPointer = 0;
	sendBuffer = nullptr;
	fileBeingSent = nullptr;
	closeRequested = false;
	nextWrite = nullptr;
	waitingForDataConnection = false;
}

// Read one char from the NetworkTransaction
bool NetworkTransaction::Read(char& b)
{
	if (LostConnection() || readingPb == nullptr)
	{
		b = 0;
		return false;
	}

	b = ((const char*)readingPb->payload)[inputPointer++];
	if (inputPointer > readingPb->len)
	{
		readingPb = readingPb->next;
		inputPointer = 0;
	}
	return true;
}

// Read data from the NetworkTransaction and return true on success
bool NetworkTransaction::ReadBuffer(const char *&buffer, unsigned int &len)
{
	if (LostConnection() || readingPb == nullptr)
	{
		return false;
	}

	if (inputPointer >= readingPb->len)
	{
		readingPb = readingPb->next;
		inputPointer = 0;
		if (readingPb == nullptr)
		{
			return false;
		}
	}

	buffer = (const char*)readingPb->payload + inputPointer;
	len = readingPb->len - inputPointer;
	readingPb = readingPb->next;
	inputPointer = 0;
	return true;
}

void NetworkTransaction::Write(char b)
{
	if (CanWrite())
	{
		if (sendBuffer == nullptr && !OutputBuffer::Allocate(sendBuffer))
		{
			// Should never get here
			return;
		}

		sendBuffer->cat(b);
	}
}

void NetworkTransaction::Write(const char* s)
{
	if (CanWrite())
	{
		if (sendBuffer == nullptr && !OutputBuffer::Allocate(sendBuffer))
		{
			// Should never get here
			return;
		}

		sendBuffer->cat(s);
	}
}

void NetworkTransaction::Write(StringRef ref)
{
	Write(ref.Pointer(), ref.strlen());
}

void NetworkTransaction::Write(const char* s, size_t len)
{
	if (CanWrite())
	{
		if (sendBuffer == nullptr && !OutputBuffer::Allocate(sendBuffer))
		{
			// Should never get here
			return;
		}

		sendBuffer->cat(s, len);
	}
}

void NetworkTransaction::Write(OutputBuffer *buffer)
{
	if (CanWrite())
	{
		// Note we use an individual stack here, because we don't want to link different
		// OutputBuffers for different destinations together...
		sendStack->Push(buffer);
	}
	else
	{
		// Don't keep buffers we can't send...
		OutputBuffer::ReleaseAll(buffer);
	}
}

void NetworkTransaction::Write(OutputStack *stack)
{
	if (stack != nullptr)
	{
		if (CanWrite())
		{
			sendStack->Append(stack);
		}
		else
		{
			stack->ReleaseAll();
		}
	}
}

void NetworkTransaction::Printf(const char* fmt, ...)
{
	if (CanWrite() && (sendBuffer != nullptr || OutputBuffer::Allocate(sendBuffer)))
	{
		va_list p;
		va_start(p, fmt);
		sendBuffer->vprintf(fmt, p);
		va_end(p);
	}
}

void NetworkTransaction::SetFileToWrite(FileStore *file)
{
	if (CanWrite())
	{
		fileBeingSent = file;
	}
	else if (file != nullptr)
	{
		file->Close();
	}
}

// Send exactly one TCP window of data and return true when done
bool NetworkTransaction::Send()
{
	// Free up this transaction if we either lost the connection or if it is supposed to be closed
	if (LostConnection() || closeRequested)
	{
		if (fileBeingSent != nullptr)
		{
			fileBeingSent->Close();
			fileBeingSent = nullptr;
		}
		OutputBuffer::ReleaseAll(sendBuffer);
		sendStack->ReleaseAll();

		if (!LostConnection())
		{
			reprap.GetNetwork()->ConnectionClosed(cs, true);
		}

		if (closingDataPort)
		{
			if (ftp_pasv_pcb != nullptr)
			{
				tcp_accept(ftp_pasv_pcb, nullptr);
				tcp_close(ftp_pasv_pcb);
				ftp_pasv_pcb = nullptr;
			}

			closingDataPort = false;
		}
		return true;
	}

	// Do we have enough space left for sending? Third-party apps may be sending data as well, so check this first
	if (tcp_sndbuf(cs->pcb) < TCP_WND)
	{
		if (reprap.Debug(moduleNetwork))
		{
			reprap.GetPlatform()->Message(HOST_MESSAGE, "Network: Could not send data because not enough sending space is available\n");
		}
		return false;
	}

	// Fill up the TCP window with some data chunks from our OutputBuffer instances
	size_t bytesBeingSent = 0, bytesLeftToSend = TCP_WND;
	while (sendBuffer != nullptr && bytesLeftToSend > 0)
	{
		size_t copyLength = min<size_t>(bytesLeftToSend, sendBuffer->BytesLeft());
		memcpy(sendingWindow() + bytesBeingSent, sendBuffer->Read(copyLength), copyLength);
		bytesBeingSent += copyLength;
		bytesLeftToSend -= copyLength;

		if (sendBuffer->BytesLeft() == 0)
		{
			sendBuffer = OutputBuffer::Release(sendBuffer);
			if (sendBuffer == nullptr)
			{
				sendBuffer = sendStack->Pop();
			}
		}
	}

	// We also intend to send a file, so check if we can fill up the TCP window
	if (sendBuffer == nullptr && bytesLeftToSend != 0 && fileBeingSent != nullptr)
	{
		// For HSMCI efficiency, read from the file in multiples of 4 bytes except at the end.
		// This ensures that the second and subsequent chunks can be DMA'd directly into sendingWindow.
		size_t bytesToRead = bytesLeftToSend & (~3);
		if (bytesToRead != 0)
		{
			int bytesRead = fileBeingSent->Read(sendingWindow() + bytesBeingSent, bytesToRead);
			if (bytesRead > 0)
			{
				bytesBeingSent += bytesRead;
			}

			if (bytesRead != (int)bytesToRead)
			{
				fileBeingSent->Close();
				fileBeingSent = nullptr;
			}
		}
	}

	if (bytesBeingSent == 0)
	{
		// If we have no data to send and fileBeingSent is nullptr, we can close the connection next time
		if (!cs->persistConnection && nextWrite == nullptr)
		{
			Close();
			return false;
		}

		// We want to send data from another transaction, so only free up this one
		return true;
	}

	// The TCP window has been filled up as much as possible, so send it now. There is no need to check
	// the available space in the SNDBUF queue, because we really write only one TCP window at once.
	err_t result = tcp_write(cs->pcb, sendingWindow(), bytesBeingSent, 0);
	if (result == ERR_OK)
	{
		result = tcp_output(cs->pcb);
	}

	if (ERR_IS_FATAL(result))
	{
		reprap.GetPlatform()->MessageF(HOST_MESSAGE, "Network: Failed to write data in Send (code %d)\n", result);
		tcp_abort(cs->pcb);
		cs->pcb = nullptr;
		return false;
	}

	tcp_poll(cs->pcb, conn_poll, TCP_WRITE_TIMEOUT / TCP_SLOW_INTERVAL / TCP_MAX_SEND_RETRIES);
	tcp_sent(cs->pcb, conn_sent);

	sendingConnection = cs;
	sendingRetries = 0;
	sendingWindowSize = sentDataOutstanding = bytesBeingSent;
	return false;
}

// This is called by the Webserer to send output data to a client. If keepConnectionAlive is set to false,
// the current connection is terminated once everything has been sent.
void NetworkTransaction::Commit(bool keepConnectionAlive)
{
	if (status == dataSending)
	{
		reprap.GetNetwork()->readyTransactions = next;
		// This transaction is already on the list of sending transactions. Pretend it's already complete
	}
	else
	{
		if (LostConnection())
		{
			// Discard transaction if no connection is available
			Discard();
		}
		else
		{
			// We intend to send data, so move this transaction and prepare some values
			FreePbuf();
			reprap.GetNetwork()->readyTransactions = next;
			cs->persistConnection = keepConnectionAlive;
			if (sendBuffer == nullptr)
			{
				sendBuffer = sendStack->Pop();
			}
			status = dataSending;

			// Enqueue this transaction, so it's sent in the right order
			NetworkTransaction *mySendingTransaction = cs->sendingTransaction;
			if (mySendingTransaction == nullptr)
			{
				cs->sendingTransaction = this;
				NetworkTransaction * volatile * writingTransactions = &reprap.GetNetwork()->writingTransactions;
				reprap.GetNetwork()->AppendTransaction(writingTransactions, this);
			}
			else
			{
				while (mySendingTransaction->nextWrite != nullptr)
				{
					mySendingTransaction = mySendingTransaction->nextWrite;
				}
				mySendingTransaction->nextWrite = this;
			}
		}
	}
}

void NetworkTransaction::Defer()
{
	// First free up the allocated pbufs
	FreePbuf();

	// Call LWIP task to process tcp_recved(). This will hopefully send an ACK
	ethernet_task();
}

// This method should be called if we don't want to send data to the client and if
// we don't want to interfere with the connection state, i.e. keep it alive.
void NetworkTransaction::Discard()
{
	// Is this the transaction we should be dealing with?
	if (reprap.GetNetwork()->readyTransactions != this)
	{
		// Should never get here
		return;
	}
	reprap.GetNetwork()->readyTransactions = next;

	// Free up some resources
	FreePbuf();
	if (fileBeingSent != nullptr)
	{
		fileBeingSent->Close();
		fileBeingSent = nullptr;
	}
	OutputBuffer::ReleaseAll(sendBuffer);
	sendStack->ReleaseAll();

	// Release this transaction unless it's still in use for sending
	if (status != dataSending)
	{
		NetworkTransaction * volatile * freeTransactions = &reprap.GetNetwork()->freeTransactions;
		reprap.GetNetwork()->AppendTransaction(freeTransactions, this);
	}

	// Call disconnect event if this transaction indicates a graceful disconnect
	if (!LostConnection() && status == disconnected)
	{
		if (reprap.Debug(moduleNetwork))
		{
			reprap.GetPlatform()->MessageF(HOST_MESSAGE, "Network: Discard() is handling a graceful disconnect for cs=%08x\n", (unsigned int)cs);
		}
		reprap.GetNetwork()->ConnectionClosed(cs, true);
	}
}

void NetworkTransaction::SetConnectionLost()
{
	cs = nullptr;
	FreePbuf();
	for (NetworkTransaction *rs = nextWrite; rs != nullptr; rs = rs->nextWrite)
	{
		rs->cs = nullptr;
	}
}

uint32_t NetworkTransaction::GetRemoteIP() const
{
	return (cs != nullptr) ? cs->pcb->remote_ip.addr : 0;
}

uint16_t NetworkTransaction::GetRemotePort() const
{
	return (cs != nullptr) ? cs->pcb->remote_port : 0;
}

uint16_t NetworkTransaction::GetLocalPort() const
{
	return (cs != nullptr) ? cs->pcb->local_port : 0;
}

void NetworkTransaction::Close()
{
	tcp_pcb *pcb = cs->pcb;
	tcp_recv(pcb, nullptr);
	closeRequested = true;
}

void NetworkTransaction::FreePbuf()
{
	// Tell LWIP that we have processed data
	if (cs != nullptr && bufferLength > 0 && cs->pcb != nullptr)
	{
		tcp_recved(cs->pcb, bufferLength);
		bufferLength = 0;
	}

	// Free pbuf (pbufs are thread-safe)
	if (pb != nullptr)
	{
		pbuf_free(pb);
		pb = readingPb = nullptr;
	}
}

// vim: ts=4:sw=4
