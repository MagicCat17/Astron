#include "MessageDirector.h"
#include "MDNetworkParticipant.h"
#include "core/global.h"
#include "core/config.h"
#include <boost/bind.hpp>
#include <boost/icl/interval_bounds.hpp>
using boost::asio::ip::tcp; // I don't want to type all of that god damned shit
ConfigVariable<std::string> bind_addr("messagedirector/bind", "unspecified");
ConfigVariable<std::string> connect_addr("messagedirector/connect", "unspecified");

// Define convenience type
typedef boost::icl::discrete_interval<channel_t> interval_t; 

bool ChannelList::qualifies(channel_t channel)
{
	if(is_range)
	{
		return (channel >= a && channel <= b);
	}
	else
	{
		return channel == a;
	}
}

bool ChannelList::operator==(const ChannelList &rhs)
{
	if(is_range && rhs.is_range)
	{
		return (a == rhs.a && b == rhs.b);
	}
	else if(!is_range && !rhs.is_range)
	{
		return a == rhs.a;
	}
	return false;
}

MessageDirector MessageDirector::singleton;

void MessageDirector::InitializeMD()
{
	if(!m_initialized)
	{
		// Bind to port and listen for downstream servers
		if(bind_addr.get_val() != "unspecified")
		{
			m_log.debug() << "binding" << std::endl;
			std::string str_ip = bind_addr.get_val();
			std::string str_port = str_ip.substr(str_ip.find(':', 0)+1, std::string::npos);
			str_ip = str_ip.substr(0, str_ip.find(':', 0));
			tcp::resolver resolver(io_service);
			tcp::resolver::query query(str_ip, str_port);
			tcp::resolver::iterator it = resolver.resolve(query);
			m_acceptor = new tcp::acceptor(io_service, *it, true);
			start_accept();
		}

		// Connect to upstream server and start handling recieved messages
		if(connect_addr.get_val() != "unspecified")
		{
			m_log.debug() << "connecting" << std::endl;
			std::string str_ip = connect_addr.get_val();
			std::string str_port = str_ip.substr(str_ip.find(':', 0)+1, std::string::npos);
			str_ip = str_ip.substr(0, str_ip.find(':', 0));
			tcp::resolver resolver(io_service);
			tcp::resolver::query query(str_ip, str_port);
			tcp::resolver::iterator it = resolver.resolve(query);
			m_remote_md = new tcp::socket(io_service);
			boost::system::error_code ec;
			m_remote_md->connect(*it, ec);
			if(ec.value() != 0)
			{
				m_log.fatal() << "Could not connect to remote MD at IP: "
				              << connect_addr.get_val() << " With error code: "
				              << ec.value() << "(" << ec.category().message(ec.value()) << ")"
				              << std::endl;
				exit(1);
			}
			m_buffer = new char[2];
			m_bufsize = 2;
			m_bytes_to_go = 2;
			start_receive();
		}

		// Initialize m_range_susbcriptions with empty range
		auto empty_set = std::set<MDParticipantInterface*>();
		m_range_subscriptions = boost::icl::interval_map<channel_t, std::set<MDParticipantInterface*>>();
		m_range_subscriptions += std::make_pair(interval_t::closed(0, ULLONG_MAX), empty_set);
	}
}

void MessageDirector::handle_datagram(Datagram *dg, MDParticipantInterface *participant)
{
	m_log.debug() << "MD Handling datagram" << std::endl;
	DatagramIterator dgi(dg);
	unsigned char channels = dgi.read_uint8();

	// Check for control messages
	if(channels == 1)
	{
		channel_t channel = dgi.read_uint64();

		if(channel == CONTROL_MESSAGE && participant)
		{
			unsigned int msg_type = dgi.read_uint16();
			switch(msg_type)
			{
				case CONTROL_ADD_CHANNEL:
				{
					subscribe_channel(participant, dgi.read_uint64());
				}
				break;
				case CONTROL_ADD_RANGE:
				{
					channel_t lo = dgi.read_uint64();
					channel_t hi = dgi.read_uint64();
					subscribe_range(participant, lo, hi);
				}
				break;
				case CONTROL_REMOVE_CHANNEL:
				{
					unsubscribe_channel(participant, dgi.read_uint64());
				}
				break;
				case CONTROL_REMOVE_RANGE:
				{
					channel_t lo = dgi.read_uint64();
					channel_t hi = dgi.read_uint64();
					unsubscribe_range(participant, lo, hi);
				}
				break;
				case CONTROL_ADD_POST_REMOVE:
				{
					std::string data = dgi.read_string();
					participant->m_post_remove = data;
				}
				break;
				case CONTROL_CLEAR_POST_REMOVE:
				{
					participant->m_post_remove = "";
				}
				break;
				default:
					m_log.error() << "Unknown MD MsgType: " << msg_type << std::endl;
			}
			return;
		}

		// return offset to beginning of recipients if not a CONTROL_MESSAGE
		dgi.seek(1);
	}

	// Route messages to participants
	std::set<MDParticipantInterface*> receiving_participants;
	for(unsigned char i = 0; i < channels; ++i)
	{
		channel_t channel = dgi.read_uint64();
		for(auto it = m_channel_subscriptions[channel].begin(); it != m_channel_subscriptions[channel].end(); ++it)
		{
			receiving_participants.insert(receiving_participants.end(), *it);
		}

		auto range = boost::icl::find(m_range_subscriptions, channel);
		if(range != m_range_subscriptions.end())
		{
			receiving_participants.insert(range->second.begin(), range->second.end());
		}
	}

	if (participant)
	{
		receiving_participants.erase(participant);
	}

	for(auto it = receiving_participants.begin(); it != receiving_participants.end(); ++it)
	{
		DatagramIterator msg_dgi(dg, 1+channels*8);
		(*it)->handle_datagram(dg, msg_dgi);
	}

	if(participant && m_remote_md)  // Send message upstream
	{
		unsigned short len = dg->get_buf_end();
		m_remote_md->send(boost::asio::buffer((char*)&len, 2));
		m_remote_md->send(boost::asio::buffer(dg->get_data(), len));
		m_log.debug() << "sending to upstream md" << std::endl;
	}
	else if(!participant) // If there is no participant, then it came from the upstream
	{
		m_log.debug() << "not sending to upstream because it came from upstream" << std::endl;
	}
	else // Otherwise is root node
	{
		m_log.debug() << "no upstream md" << std::endl;
	}
}

void MessageDirector::subscribe_channel(MDParticipantInterface* p, channel_t c)
{
	// Check if participant is already subscribed in a range
	if(boost::icl::find(p->m_ranges, c) == p->m_ranges.end()) {
		// If not, subscribe participant to channel
		p->m_channels.insert(p->m_channels.end(), c);
		m_channel_subscriptions[c].insert(m_channel_subscriptions[c].end(), p);
	}
	// Check if should subscribe upstream...
	if(m_remote_md)
	{
		// Check if there was a previous subscription on that channel
		if(m_channel_subscriptions[c].size() > 1)
		{
			return;
		}

		// Check if we are already subscribing to that channel upstream via a range
		auto range_it = boost::icl::find(m_range_subscriptions, c);
		if(range_it != m_range_subscriptions.end()  && !range_it->second.empty())
		{
			return;
		}

		// Send upstream control message
		Datagram dg(CONTROL_ADD_CHANNEL);
		dg.add_uint64(c);
		unsigned short len = dg.get_buf_end();
		m_remote_md->send(boost::asio::buffer((char*)&len, 2));
		m_remote_md->send(boost::asio::buffer(dg.get_data(), len));
	}
}

void MessageDirector::unsubscribe_channel(MDParticipantInterface* p, channel_t c)
{
	// Unsubscribe participant from channel
	p->m_channels.erase(c);
	m_channel_subscriptions[c].erase(p);

	// Check if unsubscribed channel in the middle of a range
	auto range_it = boost::icl::find(m_range_subscriptions, c); // O(log n)
	if(range_it != m_range_subscriptions.end())
	{
		auto p_it = range_it->second.find(p);
		if(p_it != range_it->second.end())
		{
			// Prepare participant and range
			std::set<MDParticipantInterface*> participant_set;
			participant_set.insert(p);

			interval_t interval = interval_t::closed(c, c);

			// Split range around channel split([a,b], n) -> [a,n) (n, b]
			m_range_subscriptions -= std::make_pair(interval, participant_set); // O(n)
		}
	}

	// Check if should unsubscribe upstream...
	if(m_remote_md) {
		// Check if there are any remaining single channel subscriptions
		if(m_channel_subscriptions[c].size() > 0)
		{
			return; // Don't unsubscribe upstream
		}

		// Check if the range containing the unsubscribed channel has subscribers
		auto range = boost::icl::find(m_range_subscriptions, c);
		if(range != m_range_subscriptions.end()  && !range->second.empty())
		{
			return; // Don't unsubscribe upstream
		}

		// Send upstream control message
		Datagram dg(CONTROL_REMOVE_CHANNEL);
		dg.add_uint64(c);
		unsigned short len = dg.get_buf_end();
		m_remote_md->send(boost::asio::buffer((char*)&len, 2));
		m_remote_md->send(boost::asio::buffer(dg.get_data(), len));
	}
}

void MessageDirector::subscribe_range(MDParticipantInterface* p, channel_t lo, channel_t hi)
{
	// Prepare participant and range
	std::set<MDParticipantInterface*> participant_set;
	participant_set.insert(p);

	interval_t interval = interval_t::closed(lo, hi);

	// Subscribe participant to range
	p->m_ranges += interval;
	m_range_subscriptions += std::make_pair(interval, participant_set);

	// Remove old subscriptions from participants where: [ range.low <= old_channel <= range.high ]
	for (auto it = p->m_channels.begin(); it != p->m_channels.end();)
	{
		auto prev = it++;
		channel_t c = *prev;

		if (lo <= c && c <= hi)
		{
			m_channel_subscriptions[c].erase(p);
			p->m_channels.erase(prev);
		}
	}

	// Check if should subscribe upstream...
	if(m_remote_md)
	{
		// Check how many intervals along that range are already subscribed
		int new_intervals = 0, premade_intervals = 0;
		auto interval_range = m_range_subscriptions.equal_range(interval_t::closed(lo, hi));
		for(auto it = interval_range.first; it != interval_range.second; ++it)
		{
			++new_intervals;
			if (it->second.size() > 1)
			{
				++premade_intervals;
			}
		}

		// If all existing intervals are already subscribed, don't upstream
		if(new_intervals == premade_intervals)
		{
			return;
		}

		// Send upstream control message
		Datagram dg(CONTROL_ADD_RANGE);
		dg.add_uint64(lo);
		dg.add_uint64(hi);
		unsigned short len = dg.get_buf_end();
		m_remote_md->send(boost::asio::buffer((char*)&len, 2));
		m_remote_md->send(boost::asio::buffer(dg.get_data(), len));
	}
}

void MessageDirector::unsubscribe_range(MDParticipantInterface *p, channel_t lo, channel_t hi)
{
	// Prepare participant and range
	std::set<MDParticipantInterface*> participant_set;
	participant_set.insert(p);

	interval_t interval = interval_t::closed(lo, hi);

	// Unsubscribe participant from range
	p->m_ranges -= interval;
	m_range_subscriptions -= std::make_pair(interval, participant_set);

	// Remove old subscriptions from participants where: [ range.low <= old_channel <= range.high ]
	for (auto it = p->m_channels.begin(); it != p->m_channels.end();)
	{
		auto prev = it++;
		channel_t c = *prev;

		if (lo <= c && c <= hi)
		{
			m_channel_subscriptions[c].erase(p);
			p->m_channels.erase(prev);
		}
	}

	m_log.debug() << "Remove Lower: " << lo << "  Upper: " << hi << std::endl;

	// Check if should unsubscribe upstream...
	if(m_remote_md)
	{
		// Declare list of intervals that are no longer subscribed too
		std::list<interval_t> silent_intervals;

		// If that was the last interval in m_range_subscriptions, remove it upstream
		if (boost::icl::interval_count(m_range_subscriptions) == 0)
		{
			silent_intervals.insert(silent_intervals.end(), interval);
		}
		else
		{
			// Check each interval range (except the last, so we can stop there)
			auto next = ++m_range_subscriptions.begin();
			for(auto it = m_range_subscriptions.begin(); it != m_range_subscriptions.end(); ++it)
			{
				// For the range we care about
				// TODO: Read Boost::ICL to find a way to restrict the iterator to a range we care about
				if(it->first.lower() <= hi && it->first.upper() >= lo) {
					// If an existing interval is empty it is silent
					if (it->second.empty())
					{
						silent_intervals.insert(silent_intervals.end(), it->first);
					}
					if(next != m_range_subscriptions.end()) {
						// If there is room between intervals, that is a silent interval
						if (next->first.lower() - it->first.upper() > 0) {
							silent_intervals.insert(silent_intervals.end(), boost::icl::inner_complement(it->first, next->first));
						}
						++next;
					}
				}
			}
		}

		// Send unsubscribe messages
		for(auto it = silent_intervals.begin(); it != silent_intervals.end(); ++it)
		{
			m_log.debug() << "Upstream remove Lower: " << it->lower() << "  Upper: " << it->upper() << std::endl;
			Datagram dg(CONTROL_REMOVE_RANGE);
			dg.add_uint64(it->lower());
			dg.add_uint64(it->upper());
			unsigned short len = dg.get_buf_end();
			m_remote_md->send(boost::asio::buffer((char*)&len, 2));
			m_remote_md->send(boost::asio::buffer(dg.get_data(), len));
		}
	}
}

MessageDirector::MessageDirector() : m_acceptor(NULL), m_initialized(false), m_remote_md(NULL),
	m_buffer(NULL), m_bufsize(0), m_bytes_to_go(0), m_is_data(false), m_log("msgdir", "Message Director")
{
}

void MessageDirector::start_accept()
{
	tcp::socket *socket = new tcp::socket(io_service);
	tcp::endpoint peerEndpoint;
	m_acceptor->async_accept(*socket, boost::bind(&MessageDirector::handle_accept, 
		this, socket, boost::asio::placeholders::error));
}

void MessageDirector::handle_accept(tcp::socket *socket, const boost::system::error_code &ec)
{
	boost::asio::ip::tcp::endpoint remote = socket->remote_endpoint();
	m_log.info() << "Got a Message Director connection from "
	             << remote.address() << ":" << remote.port() << std::endl;
	new MDNetworkParticipant(socket); //It deletes itsself when connection is lost
	start_accept();
}

void MessageDirector::start_receive()
{
	unsigned short offset = m_bufsize - m_bytes_to_go;
	m_remote_md->async_receive(boost::asio::buffer(m_buffer+offset, m_bufsize-offset), boost::bind(&MessageDirector::read_handler,
		this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
}

void MessageDirector::read_handler(const boost::system::error_code &ec, size_t bytes_transferred)
{
	if(ec.value() != 0)
	{
		m_log.fatal() << "Lost connection to remote MD error: " << ec.category().message(ec.value()) << std::endl;
		exit(1);
	}
	else
	{
		m_bytes_to_go -= bytes_transferred;
		if(m_bytes_to_go == 0)
		{
			if(!m_is_data)
			{
				m_bufsize = *(unsigned short*)m_buffer;
				delete [] m_buffer;
				m_buffer = new char[m_bufsize];
				m_bytes_to_go = m_bufsize;
				m_is_data = true;
			}
			else
			{
				Datagram dg(std::string(m_buffer, m_bufsize));
				delete [] m_buffer;//dg makes a copy
				m_bufsize = 2;
				m_buffer = new char[m_bufsize];
				m_bytes_to_go = m_bufsize;
				m_is_data = false;

				m_log.debug() << "Reader handle datagram" << endl;
				handle_datagram(&dg, NULL);
			}
		}

		start_receive();
	}
}

void MessageDirector::add_participant(MDParticipantInterface* p)
{
	m_participants.insert(m_participants.end(), p);
}

void MessageDirector::remove_participant(MDParticipantInterface* p)
{
	// Send out a post remove, if one exists
	if(p->m_post_remove.length() > 0)
	{
		Datagram dg(p->m_post_remove);
		handle_datagram(&dg, p);
	}

	// Send out unsubscribe messages for indivually subscribed channels
	auto channels = std::set<channel_t>(p->m_channels);
	for(auto it = channels.begin(); it != channels.end(); ++it)
	{
		channel_t channel = *it;
		unsubscribe_channel(p, channel);
	}

	// Send out unsubscribe messages for subscribed channel ranges
	auto ranges = boost::icl::interval_set<channel_t>(p->m_ranges);
	for(auto it = ranges.begin(); it != ranges.end(); ++it)
	{
		unsubscribe_range(p, it->lower(), it->upper());
	}

	// Stop tracking participant
	m_participants.remove(p);
}
