#!../../bin/linux-x86_64/tableExampleIoc

< envPaths

dbLoadDatabase("$(TOP)/dbd/tableExampleIoc.dbd")
tableExampleIoc_registerRecordDeviceDriver(pdbbase)

dbLoadRecords("$(TOP)/db/table-csv.db")
dbLoadRecords("$(TOP)/db/table-csv2.db")
dbLoadRecords("$(TOP)/db/table-sim.db")
dbLoadRecords("$(TOP)/db/table-soft.db")
dbLoadRecords("$(TOP)/db/table-stat.db")
dbLoadRecords("$(TOP)/db/table-src-rows.db")
dbLoadRecords("$(TOP)/db/table-csv2-rows.db")

iocInit()
