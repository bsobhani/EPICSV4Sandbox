/* NeutronServerMain.cpp
 *
 * Copyright (c) 2014 Oak Ridge National Laboratory.
 * All rights reserved.
 * See file LICENSE that is included with this distribution.
 *
 * Based on MRK pvDataBaseCPP exampleServer
 *
 * @author Kay Kasemir
 */
#include <cstddef>
#include <cstdlib>
#include <cstddef>
#include <string>
#include <cstdio>
#include <memory>
#include <iostream>
#include <unistd.h>

#include <pv/standardField.h>
#include <pv/standardPVField.h>
#include "neutronServer.h"
#include <pv/traceRecord.h>
#include <pv/channelProviderLocal.h>
#include <pv/serverContext.h>

#include <pv/createRequest.h>


using namespace std;
using std::tr1::shared_ptr;
using namespace epics::pvData;
using namespace epics::pvAccess;
using namespace epics::pvDatabase;
using namespace epics::neutronServer;

static void help(const char *name)
{
    cout << "USAGE: " << name << " [options]" << endl;
    cout << "  -h        : Help" << endl;
    cout << "  -d seconds: Delay between packages (default 0.01)" << endl;
    cout << "  -e count  : Max event count per packet (default 10)" << endl;
    cout << "  -m : Random event count, using 'count' as maximum" << endl;
    cout << "  -r : Generate normally distributed data which looks semi realistic." << endl;
    cout << "  -s Nth : Don't send every N'th packet to simulate losing data packets (default 0 which means disabled)." << endl;
}

int main(int argc,char *argv[])
{
    double delay = 0.01;
    size_t event_count = 10;
    bool random_count = false;
    bool realistic = false;
    size_t skip_packets = 0;

    int opt;
    while ((opt = getopt(argc, argv, "d:e:h:mrs:")) != -1)
    {
        switch (opt)
        {
        case 'd':
            delay = atof(optarg);
            break;
        case 'e':
            event_count = (size_t)atol(optarg);
            break;
        case 'h':
            help(argv[0]);
            return 0;
        case 'm':
        	random_count = true;
            break;
        case 'r':
        	realistic = true;
                break;
        case 's':
                skip_packets = (size_t)atol(optarg);
                break;
        default:
            help(argv[0]);
            return -1;
        }
    }

    cout << "Delay : " << delay << " seconds" << endl;
    cout << "Events: " << event_count << endl;
    cout << "Realistic: " << realistic << endl;
    if (skip_packets > 0) {
      cout << "Skipping every " << skip_packets << " packets." << endl;
    }

    PVDatabasePtr master = PVDatabase::getMaster();
    ChannelProviderLocalPtr channelProvider = getChannelProviderLocal();


    NeutronPVRecord::shared_pointer neutrons = NeutronPVRecord::create("neutrons");
    if (! master->addRecord(neutrons))
        throw std::runtime_error("Cannot add record " + neutrons->getRecordName());

    shared_ptr<FakeNeutronEventRunnable> runnable(new FakeNeutronEventRunnable(neutrons, delay, event_count, random_count, realistic, skip_packets));
    shared_ptr<epicsThread> thread(new epicsThread(*runnable, "processor", epicsThreadGetStackSize(epicsThreadStackMedium)));
    thread->start();

    PVRecordPtr pvRecord = TraceRecord::create("traceRecordPGRPC");
    if (! master->addRecord(pvRecord))
        throw std::runtime_error("Cannot add record " + pvRecord->getRecordName());
    // Release record, held by database
    pvRecord.reset();

    ServerContext::shared_pointer pvaServer = 
        startPVAServer(PVACCESS_ALL_PROVIDERS,0,true,true);
    cout << "neutronServer running\n";
    string str;
    while(true) {
        cout << "Type exit to stop: \n";
        getline(cin,str);
        if(str.compare("exit")==0) break;

    }
    runnable->shutdown();
    pvaServer->shutdown();
    epicsThreadSleep(1.0);
    // pvaServer->destroy();
    // channelProvider->destroy();

    return 0;
}

