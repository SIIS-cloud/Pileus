#!/usr/bin/env python2.7

from SimpleXMLRPCServer import SimpleXMLRPCServer
from SimpleXMLRPCServer import SimpleXMLRPCRequestHandler

class RequestHandler(SimpleXMLRPCRequestHandler):
    rpc_paths = ('/RPC2',)

server = SimpleXMLRPCServer(("localhost", 8000),
                            requestHandler=RequestHandler,logRequests=False)

server.register_introspection_functions()

def test(arg):
    return arg

server.register_function(test)

server.serve_forever()


