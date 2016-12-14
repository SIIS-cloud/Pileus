#!/usr/bin/env python3

import socketserver
import ssl
import xmlrpc.server

try:
    import fcntl
except ImportError:
    fcntl = None


class SecureXMLRPCServer(socketserver.TCPServer, xmlrpc.server.SimpleXMLRPCDispatcher):
    allow_reuse_address = True
    def __init__(self, addr, certfile, keyfile=None, requestHandler=xmlrpc.server.SimpleXMLRPCRequestHandler,
                logRequests=False, allow_none=False, encoding=None, bind_and_activate=True, ssl_version=ssl.PROTOCOL_TLSv1):
        self.logRequests = logRequests

        self.context = ssl.SSLContext(ssl_version)
        self.context.load_cert_chain(certfile=certfile, keyfile=keyfile)

        xmlrpc.server.SimpleXMLRPCDispatcher.__init__(self, allow_none, encoding)

        socketserver.TCPServer.__init__(self, addr, requestHandler, bind_and_activate)

        if fcntl is not None and hasattr(fcntl, 'FD_CLOEXEC'):
            flags = fcntl.fcntl(self.fileno(), fcntl.F_GETFD)
            flags |= fcntl.FD_CLOEXEC
            fcntl.fcntl(self.fileno(), fcntl.F_SETFD, flags)

    def get_request(self):
        newsocket, fromaddr = self.socket.accept()
        sslsocket = self.context.wrap_socket(newsocket, server_side=True)
        return sslsocket, fromaddr


def test(arg):
    #print(arg)
    return arg

server = SecureXMLRPCServer(('localhost', 8000), 'cert.pem')
server.register_introspection_functions()
server.register_function(pow)
server.register_function(test)
server.serve_forever()
