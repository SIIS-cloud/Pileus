#!/usr/bin/env python3
from multiprocessing import Process, Value
import time
import sys
import xmlrpc.client

def call_rpc(errors, i, num):
    try:
        for j in range(0, num):
            s = xmlrpc.client.ServerProxy('https://localhost:8000')
            s.test(i)
    except Exception as Ex:
        errors.value += 1

def jobs_process(errors, process_n, num):
    for i in range(process_n):
        p = Process(target=call_rpc, args=(errors, i, num))
        p.start()

if __name__ == '__main__':
    process_n, num_n = sys.argv[1:]
    errors = Value('i', 0)
    intprocess = int(process_n)
    num = int(num_n)
    start = time.time()
    jobs = Process(target=jobs_process, args=(errors, intprocess, num))
    jobs.start()
    jobs.join()
    took = time.time() - start
    print("Total jobs: %s" % (process_n))
    print("RPC Errors: %s" % (errors.value))
    print("Elapsed time: %s" % (took))
    sys.exit(0)

        
        
