/* neutronServerRegister.cpp
 *
 * Copyright (c) 2014 Oak Ridge National Laboratory.
 * All rights reserved.
 * See file LICENSE that is included with this distribution.
 *
 * Based on MRK pvDataBaseCPP exampleServer
 *
 * @author Kay Kasemir
 */
#include <iostream>

#include <iocsh.h>
#include <epicsExport.h>

#include <pv/pvDatabase.h>

#include <neutronServer.h>

using namespace std;
using namespace epics::pvDatabase;
using namespace epics::neutronServer;

static const iocshArg createArg0 = { "recordName", iocshArgString };
static const iocshArg createArg1 = { "updateDelaySecs", iocshArgDouble };
static const iocshArg createArg2 = { "eventCount", iocshArgInt };
static const iocshArg createArg3 = { "randomCount", iocshArgInt };
static const iocshArg createArg4 = { "realistic", iocshArgInt };
static const iocshArg createArg5 = { "skipPackets", iocshArgInt };
static const iocshArg *createArgs[] = { &createArg0, &createArg1, &createArg2, &createArg3, &createArg4, &createArg5 };
static const iocshFuncDef createFuncDef = { "neutronServerCreateRecord", 6, createArgs};
static void createFunc(const iocshArgBuf *args)
{
    char *name = args[0].sval;
    double delay = args[1].dval;
    size_t event_count = args[2].ival;
    bool random_count = args[3].ival;
    bool realistic = args[4].ival;
    size_t skip_packets = args[5].ival;

    NeutronPVRecord::shared_pointer record = NeutronPVRecord::create(name);
    if (! PVDatabase::getMaster()->addRecord(record))
        cout << "Cannot create neutron record '" << name << "'" << endl;

    if (delay > 0)
    {
      epicsThreadRunable *runnable = new FakeNeutronEventRunnable(record, delay, event_count, random_count, realistic, skip_packets);
        epicsThread *thread = new epicsThread(*runnable, "FakeNeutrons", epicsThreadGetStackSize(epicsThreadStackMedium));
        thread->start();
    }
}

static void neutronServerRegister(void)
{
    static int times = 0;
    if (++times == 1)
        iocshRegister(&createFuncDef, createFunc);
    else
        cout << "neutronServerRegister called " << times << " times" << endl;
}

extern "C"
{
    epicsExportRegistrar(neutronServerRegister);
}
