#include "stdafx.h"
#include "Comm5TCP.h"
#include "../main/localtime_r.h"
#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/RFXtrx.h"

#define RETRY_DELAY 30
#define Max_Comm5_MA_Relais 16

/*
   This driver allows Domoticz to control any I/O module from the MA-5xxx-2 Family, including the fiber-optics (FX)
   variations.
   https://www.comm5.com.br/en/products/io-modules/
   These modules provide relays and digital sensors in the range of 5-30V DC or 70-240V AC selectable by dip switches
   for each individual input.

*/

static inline std::string &rtrim(std::string &s) {
	s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
	return s;
}

static inline std::vector<std::string> tokenize(const std::string &s) {
	std::vector<std::string> tokens;
	std::istringstream iss(s);
	std::copy(std::istream_iterator<std::string>(iss),
		std::istream_iterator<std::string>(),
		std::back_inserter(tokens));
	return tokens;
}

static inline bool startsWith(const std::string &haystack, const std::string &needle) {
	return needle.length() <= haystack.length()
		&& std::equal(needle.begin(), needle.end(), haystack.begin());
}

Comm5TCP::Comm5TCP(const int ID, const std::string &IPAddress, const unsigned short usIPPort) :
m_szIPAddress(IPAddress)
{
	m_HwdID=ID;
	m_usIPPort=usIPPort;
	lastKnownSensorState = 0;
	initSensorData = true;
	notificationEnabled = false;
	m_bReceiverStarted = false;
}

bool Comm5TCP::StartHardware()
{
	m_bReceiverStarted = false;

	//force connect the next first time
	m_bIsStarted = true;

	//Start worker thread
	m_thread = std::make_shared<std::thread>(&Comm5TCP::Do_Work, this);
	SetThreadName(m_thread->native_handle(), "Comm5TCP");

	_log.Log(LOG_STATUS, "Comm5 MA-5XXX: Started");

	return (m_thread != nullptr);
}

bool Comm5TCP::StopHardware()
{
	if (m_thread)
	{
		RequestStop();
		m_thread->join();
		m_thread.reset();
	}
	m_bIsStarted = false;
	return true;
}

void Comm5TCP::OnConnect()
{
	_log.Log(LOG_STATUS, "Comm5 MA-5XXX: connected to: %s:%d", m_szIPAddress.c_str(), m_usIPPort);
	m_bIsStarted = true;
	notificationEnabled = false;

	sOnConnected(this);
	queryRelayState();
	querySensorState();
	enableNotifications();
}

void Comm5TCP::OnDisconnect()
{
	_log.Log(LOG_ERROR, "Comm5 MA-5XXX: disconected");
}

void Comm5TCP::Do_Work()
{
	bool bFirstTime = true;
	int count = 0;
	while (!IsStopRequested(40))
	{
		m_LastHeartbeat = mytime(NULL);
		if (bFirstTime)
		{
			bFirstTime = false;
			if (!isConnected())
			{
				connect(m_szIPAddress, m_usIPPort);
			}
		}
		else
		{
			update();
			if (count++ >= 100) {
				count = 0;
				querySensorState();
			}
		}
	}
	terminate();

	_log.Log(LOG_STATUS, "Comm5 MA-5XXX: TCP/IP Worker stopped...");
}

void Comm5TCP::processSensorData(const std::string& line)
{
	std::vector<std::string> tokens = tokenize(line);
	if (tokens.size() < 2)
		return;

	unsigned int sensorbitfield = ::strtol(tokens[1].c_str(), 0, 16);
	for (int i = 0; i < 16; ++i) {
		bool on = (sensorbitfield & (1 << i)) != 0 ? true : false;
		if (((lastKnownSensorState & (1 << i)) ^ (sensorbitfield & (1 << i))) || initSensorData) {
			SendSwitchUnchecked((i + 1) << 8, 1, 255, on, 0, "Sensor " + std::to_string(i + 1));
		}
	}
	lastKnownSensorState = sensorbitfield;
	initSensorData = false;
}

void Comm5TCP::ParseData(const unsigned char* data, const size_t len)
{
	buffer.append((const char*)data, len);

	std::stringstream stream(buffer);
	std::string line;

	while (std::getline(stream, line, '\n')) {
		line = rtrim(line);
		if (startsWith(line, "211")) {
			std::vector<std::string> tokens = tokenize(line);
			if (tokens.size() < 2)
				break;

			unsigned int relaybitfield = ::strtol(tokens[1].c_str(), 0, 16);
			for (int i = 0; i < 16; ++i) {
				bool on = (relaybitfield & (1 << i)) != 0 ? true : false;
				SendSwitch(i + 1, 1, 255, on, 0, "Relay " + std::to_string(i + 1));
			}
		}
		else if (startsWith(line, "210") && (!startsWith(line, "210 OK"))) {
			processSensorData(line);
		}
	}

	// Trim consumed bytes.
	buffer.erase(0, buffer.length() - static_cast<unsigned int>(stream.rdbuf()->in_avail()));
}

void Comm5TCP::queryRelayState()
{
	write("OUTPUTS\n");
}

void Comm5TCP::querySensorState()
{
	write("QUERY\n");
}

void Comm5TCP::enableNotifications()
{
	write("NOTIFY ON\n");
	notificationEnabled = true;
}

bool Comm5TCP::WriteToHardware(const char *pdata, const unsigned char /*length*/)
{
	const tRBUF *pSen = reinterpret_cast<const tRBUF*>(pdata);

	unsigned char packettype = pSen->ICMND.packettype;
	//unsigned char subtype = pSen->ICMND.subtype;

	if (!isConnected())
		return false;

	if (packettype == pTypeLighting2 && pSen->LIGHTING2.id3 == 0)
	{
		//light command

		int Relay = pSen->LIGHTING2.id4;
		if (Relay > Max_Comm5_MA_Relais)
			return false;

		if (pSen->LIGHTING2.cmnd == light2_sOff)
			write("RESET " + std::to_string(Relay) + '\n');
		else
			write("SET " + std::to_string(Relay) + '\n');

		return true;
	}
	return false;
}

void Comm5TCP::OnData(const unsigned char *pData, size_t length)
{
	ParseData(pData, length);
}

void Comm5TCP::OnError(const std::exception e)
{
	_log.Log(LOG_ERROR, "Comm5 MA-5XXX: Error: %s", e.what());
}

void Comm5TCP::OnError(const boost::system::error_code& error)
{
	switch (error.value())
	{
	case boost::asio::error::address_in_use:
	case boost::asio::error::connection_refused:
	case boost::asio::error::access_denied:
	case boost::asio::error::host_unreachable:
	case boost::asio::error::timed_out:
		_log.Log(LOG_ERROR, "Comm5 MA-5XXX: Can not connect to: %s:%d", m_szIPAddress.c_str(), m_usIPPort);
		break;
	case boost::asio::error::eof:
	case boost::asio::error::connection_reset:
		_log.Log(LOG_ERROR, "Comm5 MA-5XXX: Connection reset!");
		break;
	default:
		_log.Log(LOG_ERROR, "Comm5 MA-5XXX: %s", error.message().c_str());
	}
}
