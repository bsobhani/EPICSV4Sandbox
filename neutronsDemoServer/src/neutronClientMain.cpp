/* neutronClientMain.cpp
 *
 * Copyright (c) 2014 Oak Ridge National Laboratory.
 * All rights reserved.
 * See file LICENSE that is included with this distribution.
 *
 * @author Kay Kasemir
 */
#include <iostream>
#include <getopt.h>

#include <epicsThread.h>
#include <epicsTime.h>
#include <pv/epicsException.h>
#include <pv/createRequest.h>
#include <pv/event.h>
#include <pv/pvData.h>
#include <pv/clientFactory.h>
#include <pv/pvAccess.h>
#include <pv/monitor.h>

// #define TIME_IT
#ifdef TIME_IT
#include "nanoTimer.h"
#endif

using namespace std;
using namespace std::tr1;
using namespace epics::pvData;
using namespace epics::pvAccess;

/** Requester implementation,
 *  used as base for all the following *Requester
 */
class MyRequester : public virtual Requester
{
    string requester_name;
public:
    MyRequester(string const &requester_name)
    : requester_name(requester_name)
    {}

    string getRequesterName()
    {   return requester_name; }

    void message(string const & message, MessageType messageType);
};

void MyRequester::message(string const & message, MessageType messageType)
{
    cout << getMessageTypeName(messageType) << ": "
         << requester_name << " "
         << message << endl;
}

/** Requester for channel and status updates */
class MyChannelRequester : public virtual MyRequester, public virtual ChannelRequester
{
    Event connect_event;
public:
    MyChannelRequester() : MyRequester("MyChannelRequester")
    {}

    void channelCreated(const Status& status, Channel::shared_pointer const & channel);
    void channelStateChange(Channel::shared_pointer const & channel, Channel::ConnectionState connectionState);

    boolean waitUntilConnected(double timeOut)
    {
        return connect_event.wait(timeOut);
    }
};

void MyChannelRequester::channelCreated(const Status& status, Channel::shared_pointer const & channel)
{
    cout << channel->getChannelName() << " created, " << status << endl;
}

void MyChannelRequester::channelStateChange(Channel::shared_pointer const & channel, Channel::ConnectionState connectionState)
{
    cout << channel->getChannelName() << " state: "
         << Channel::ConnectionStateNames[connectionState]
         << " (" << connectionState << ")" << endl;
    if (connectionState == Channel::CONNECTED)
        connect_event.signal();
}

/** Requester for 'getting' a single value */
class MyChannelGetRequester : public virtual MyRequester, public virtual ChannelGetRequester
{
    Event done_event;
public:
    MyChannelGetRequester() : MyRequester("MyChannelGetRequester")
    {}

    void channelGetConnect(const Status& status,
            ChannelGet::shared_pointer const & channelGet,
            Structure::const_shared_pointer const & structure);

    void getDone(const Status& status,
            ChannelGet::shared_pointer const & channelGet,
            PVStructure::shared_pointer const & pvStructure,
            BitSet::shared_pointer const & bitSet);

    boolean waitUntilDone(double timeOut)
    {
        return done_event.wait(timeOut);
    }
};

void MyChannelGetRequester::channelGetConnect(const Status& status,
        ChannelGet::shared_pointer const & channelGet,
        Structure::const_shared_pointer const & structure)
{
    // Could inspect or memorize the channel's structure...
    if (status.isSuccess())
    {
        cout << "ChannelGet for " << channelGet->getChannel()->getChannelName()
             << " connected, " << status << endl;
        structure->dump(cout);

        channelGet->get();
    }
    else
    {
        cout << "ChannelGet for " << channelGet->getChannel()->getChannelName()
             << " problem, " << status << endl;
        done_event.signal();
    }
}

void MyChannelGetRequester::getDone(const Status& status,
        ChannelGet::shared_pointer const & channelGet,
        PVStructure::shared_pointer const & pvStructure,
        BitSet::shared_pointer const & bitSet)
{
    cout << "ChannelGet for " << channelGet->getChannel()->getChannelName()
         << " finished, " << status << endl;

    if (status.isSuccess())
    {
        pvStructure->dumpValue(cout);
        done_event.signal();
    }
}

/** Requester for 'monitoring' value changes of a channel */
class MyMonitorRequester : public virtual MyRequester, public virtual MonitorRequester
{
	int limit;
    bool quiet;
    Event done_event;

    epicsTime next_run;
#   ifdef TIME_IT
    NanoTimer value_timer;
#   endif
    size_t user_tag_offset;
    size_t tof_offset;
    size_t pixel_offset;
    int monitors;
    uint64 updates;
    uint64 overruns;
    uint64 last_pulse_id;
    uint64 missing_pulses;
    uint64 array_size_differences;

    void checkUpdate(shared_ptr<PVStructure> const &structure);
public:
    MyMonitorRequester(int limit, bool quiet)
    : MyRequester("MyMonitorRequester"),
      limit(limit), quiet(quiet),
      next_run(epicsTime::getCurrent()),
      user_tag_offset(-1), tof_offset(-1), pixel_offset(-1),
      monitors(0), updates(0), overruns(0), last_pulse_id(0), missing_pulses(0), array_size_differences(0)
    {}

    void monitorConnect(Status const & status, MonitorPtr const & monitor, StructureConstPtr const & structure);
    void monitorEvent(MonitorPtr const & monitor);
    void unlisten(MonitorPtr const & monitor);

    boolean waitUntilDone()
    {
        return done_event.wait();
    }
};

void MyMonitorRequester::monitorConnect(Status const & status, MonitorPtr const & monitor, StructureConstPtr const & structure)
{
    cout << "Monitor connects, " << status << endl;
    if (status.isSuccess())
    {
        // Check the structure by using only the Structure API?
        // Need to navigate the hierarchy, won't get the overall PVStructure offset.
        // Easier: Create temporary PVStructure
        PVStructurePtr pvStructure = getPVDataCreate()->createPVStructure(structure);
        shared_ptr<PVInt> user_tag = pvStructure->getSubField<PVInt>("timeStamp.userTag");
        if (! user_tag)
        {
            cout << "No 'timeStamp.userTag'" << endl;
            return;
        }
        user_tag_offset = user_tag->getFieldOffset();

        shared_ptr<PVUIntArray> tof = pvStructure->getSubField<PVUIntArray>("time_of_flight.value");
        if (! tof)
        {
            cout << "No 'time_of_flight'" << endl;
            return;
        }
        tof_offset = tof->getFieldOffset();

        shared_ptr<PVUIntArray> pixel = pvStructure->getSubField<PVUIntArray>("pixel.value");
        if (! pixel)
        {
            cout << "No 'pixel'" << endl;
            return;
        }
        pixel_offset = pixel->getFieldOffset();

        // pvStructure is disposed; keep value_offset to read data from monitor's pvStructure

        monitor->start();
    }
}

void MyMonitorRequester::monitorEvent(MonitorPtr const & monitor)
{
    shared_ptr<MonitorElement> update;
    while ((update = monitor->poll()))
    {
        // TODO Simulate slow client -> overruns on client side
        // epicsThreadSleep(0.1);

        ++updates;
        checkUpdate(update->pvStructurePtr);
        // update->changedBitSet indicates which elements have changed.
        // update->overrunBitSet indicates which elements have changed more than once,
        // i.e. we missed one (or more !) updates.
        if (! update->overrunBitSet->isEmpty())
            ++overruns;
        if (quiet)
        {
            epicsTime now(epicsTime::getCurrent());
            if (now >= next_run)
            {
                double received_perc = 100.0 * updates / (updates + missing_pulses);
                cout << updates << " updates, "
                     << overruns << " overruns, "
                     << missing_pulses << " missing pulses, "
                     << array_size_differences << " array size differences, "
                     << "received " << fixed << setprecision(1) << received_perc << "%"
                     << endl;
                overruns = 0;
                missing_pulses = 0;
                updates = 0;
                array_size_differences = 0;

#               ifdef TIME_IT
                cout << "Time for value lookup: " << value_timer << endl;
#               endif

                next_run = now + 10.0;
            }
        }
        else
        {
            cout << "Monitor:\n";

            cout << "Changed: " << *update->changedBitSet.get() << endl;
            cout << "Overrun: " << *update->overrunBitSet.get() << endl;

            update->pvStructurePtr->dumpValue(cout);
            cout << endl;
        }
        monitor->release(update);
    }
    ++ monitors;
    if (limit > 0  &&  monitors >= limit)
    {
    	cout << "Received " << monitors << " monitors" << endl;
    	done_event.signal();
    }
}

void MyMonitorRequester::checkUpdate(shared_ptr<PVStructure> const &pvStructure)
{
#   ifdef TIME_IT
    value_timer.start();
#   endif

    // Time for value lookup when re-using offset: 2us
    shared_ptr<PVInt> value = dynamic_pointer_cast<PVInt>(pvStructure->getSubField(user_tag_offset));

    // Compare: Time for value lookup when using name: 12us
    // shared_ptr<PVInt> value = pvStructure->getIntField("timeStamp.userTag");
    if (! value)
    {
        cout << "No 'timeStamp.userTag'" << endl;
        return;
    }

#   ifdef TIME_IT
    value_timer.stop();
#   endif

    // Check pulse ID for skipped updates
    uint64 pulse_id = static_cast<uint64>(value->get());
    if (last_pulse_id != 0)
    {
        int missing = pulse_id - 1 - last_pulse_id;
        if (missing > 0)
            missing_pulses += missing;
    }
    last_pulse_id = pulse_id;

    // Compare lengths of tof and pixel arrays
    shared_ptr<PVUIntArray> tof = dynamic_pointer_cast<PVUIntArray>(pvStructure->getSubField(tof_offset));
    if (!tof)
    {
        cout << "No 'time_of_flight' array" << endl;
        return;
    }

    shared_ptr<PVUIntArray> pixel = dynamic_pointer_cast<PVUIntArray>(pvStructure->getSubField(pixel_offset));
    if (!pixel)
    {
        cout << "No 'pixel' array" << endl;
        return;
    }

    if (tof->getLength() != pixel->getLength())
    {
        ++array_size_differences;
        if (! quiet)
        {
            cout << "time_of_flight: " << tof->getLength() << " elements" << endl;
            shared_vector<const uint32> tof_data;
            tof->getAs(tof_data);
            cout << tof_data << endl;

            cout << "pixel: " << pixel->getLength() << " elements" << endl;
            shared_vector<const uint32> pixel_data;
            pixel->getAs(pixel_data);
            cout << pixel_data << endl;
        }
    }
}

void MyMonitorRequester::unlisten(MonitorPtr const & monitor)
{
    cout << "Monitor unlistens" << endl;
}


/** Connect, get value, disconnect */
void getValue(string const &name, string const &request, double timeout)
{
    ChannelProvider::shared_pointer channelProvider =
            ChannelProviderRegistry::clients()->getProvider("pva");
	//getChannelProviderRegistry()->getProvider("pva"); This is the old way.
    
    if (! channelProvider)
        THROW_EXCEPTION2(runtime_error, "No channel provider");

    shared_ptr<MyChannelRequester> channelRequester(new MyChannelRequester());
    shared_ptr<Channel> channel(channelProvider->createChannel(name, channelRequester));
    channelRequester->waitUntilConnected(timeout);

    shared_ptr<PVStructure> pvRequest = CreateRequest::create()->createRequest(request);
    shared_ptr<MyChannelGetRequester> channelGetRequester(new MyChannelGetRequester());

    // This took me 3 hours to figure out:
    shared_ptr<ChannelGet> channelGet = channel->createChannelGet(channelGetRequester, pvRequest);
    // We don't care about the value of 'channelGet', so why assign it to a variable?
    // But when we _don't_ assign it to a shared_ptr<>, the one
    // returned from channel->createChannelGet() will be deleted
    // right away, and then the server(!) crashes because it receives a NULL GetRequester...

    channelGetRequester->waitUntilDone(timeout);
}

/** Monitor values */
void doMonitor(string const &name, string const &request, double timeout, short priority, int limit, bool quiet)
{
    ChannelProvider::shared_pointer channelProvider =
            ChannelProviderRegistry::clients()->getProvider("pva");
            //getChannelProviderRegistry()->getProvider("pva");
    if (! channelProvider)
        THROW_EXCEPTION2(runtime_error, "No channel provider");

    shared_ptr<MyChannelRequester> channelRequester(new MyChannelRequester());
    shared_ptr<Channel> channel(channelProvider->createChannel(name, channelRequester, priority));
    channelRequester->waitUntilConnected(timeout);

    shared_ptr<PVStructure> pvRequest = CreateRequest::create()->createRequest(request);
    shared_ptr<MyMonitorRequester> monitorRequester(new MyMonitorRequester(limit, quiet));

    shared_ptr<Monitor> monitor = channel->createMonitor(monitorRequester, pvRequest);

    // Wait until limit or forever..
    monitorRequester->waitUntilDone();

    // What to do for graceful shutdown of monitor?
    Status stat = monitor->stop();
    if (! stat.isSuccess())
    	cout << "Cannot stop monitor, " << stat << endl;
    monitor->destroy();
    channel->destroy();
}


static void help(const char *name)
{
    cout << "USAGE: " << name << " [options] [channel]" << endl;
    cout << "  -h         : Help" << endl;
    cout << "  -m         : Monitor instead of get" << endl;
    cout << "  -q         : .. quietly monitor, don't print data" << endl;
    cout << "  -r request : Request" << endl;
    cout << "  -w seconds : Wait timeout" << endl;
    cout << "  -p priority: Priority, 0..99, default 0" << endl;
    cout << "  -l monitors: Limit runtime to given number of monitors, then quit" << endl;
}

int main(int argc,char *argv[])
{
    string channel = "neutrons";
    string request = "record[queueSize=100]field()";
    double timeout = 2.0;
    bool monitor = false;
    bool quiet = false;
    short priority = ChannelProvider::PRIORITY_DEFAULT;
    int limit = 0;

    int opt;
    while ((opt = getopt(argc, argv, "r:w:p:l:mqh")) != -1)
    {
        switch (opt)
        {
        case 'r':
            request = optarg;
            break;
        case 'w':
            timeout = atof(optarg);
            break;
        case 'p':
            priority = atoi(optarg);
            break;
        case 'l':
        	limit = atoi(optarg);
            break;
        case 'm':
            monitor = true;
            break;
        case 'q':
            quiet = true;
            break;
        case 'h':
            help(argv[0]);
            return 0;
        default:
            help(argv[0]);
            return -1;
        }
    }
    if (optind < argc)
        channel = argv[optind];

    cout << "Channel:  " << channel << endl;
    cout << "Request:  " << request << endl;
    cout << "Wait:     " << timeout << " sec" << endl;
    cout << "Priority: " << priority << endl;
    cout << "Limit: " << limit << endl;

    try
    {
        ClientFactory::start();
        if (monitor)
            doMonitor(channel, request, timeout, priority, limit, quiet);
        else
            getValue(channel, request, timeout);
        ClientFactory::stop();
    }
    catch (exception &ex)
    {
        fprintf(stderr, "Exception: %s\n", ex.what());
        PRINT_EXCEPTION2(ex, stderr);
        cout << SHOW_EXCEPTION(ex);
    }

    return 0;
}
