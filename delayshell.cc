/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
// to run arping to send packet to ingress to/from machine with ip address addr  --> arping -I ingress addr

#include <sys/socket.h>
#include <pwd.h>
#include <paths.h>
#include <fstream>
#include <stdlib.h>

#include "tundevice.hh"
#include "exception.hh"
#include "ferry.hh"
#include "child_process.hh"
#include "system_runner.hh"
#include "nat.hh"
#include "socket.hh"
#include "address.hh"

using namespace std;

/* get name of the user's shell */
string shell_path( void )
{
    struct passwd *pw = getpwuid( getuid() );
    if ( pw == nullptr ) {
        throw Exception( "getpwuid" );
    }

    string shell_path( pw->pw_shell );
    if ( shell_path.empty() ) { /* empty shell means Bourne shell */
      shell_path = _PATH_BSHELL;
    }

    return shell_path;
}

int main( int argc, char *argv[] )
{
    try {
	if ( argc != 2) {
            cerr << "Please enter only the one-way delay in milliseconds" << endl;
            return EXIT_FAILURE;
	}
	const uint64_t delay_ms = atoi( argv[1] );
        ifstream input;
        input.open("/proc/sys/net/ipv4/ip_forward");
        string line;
        getline( input, line );
        if ( line != "1" ) {
            cerr << "Run \"sudo sysctl -w net.ipv4.ip_forward=1\" to enable IP forwarding" << endl;
            return EXIT_FAILURE;
        }

        /* make pair of connected sockets */
        int pipes[ 2 ];
        if ( socketpair( AF_UNIX, SOCK_DGRAM, 0, pipes ) < 0 ) {
            throw Exception( "socketpair" );
        }
        FileDescriptor egress_socket( pipes[ 0 ], "socketpair" ),
            ingress_socket( pipes[ 1 ], "socketpair" );

        /* set egress and ingress ip addresses */
        string egress_addr = "172.30.100.100";
        string ingress_addr = "172.30.100.101";

        /* Set protocol and address enumerators */
        Socket::protocol sock_udp = Socket::UDP;
        Socket::protocol sock_tcp = Socket::TCP;
        Address::protocol addr_udp = Address::UDP;
        Address::protocol addr_tcp = Address::TCP;

        TunDevice egress_tun( "egress", egress_addr, ingress_addr );

        /* create outside listener socket for UDP dns requests */
        Socket listener_socket_outside( sock_udp );
        listener_socket_outside.bind( Address( egress_addr, "0", addr_udp ) );

        /* store port outside listener socket bound to so inside socket can connect to it */
        string listener_outside_port = to_string( listener_socket_outside.local_addr().port() );

        /* address of outside dns server for UDP requests */
        Address connect_addr_outside( "localhost", "domain", addr_udp );

        /* create outside listener socket for TCP dns requests */
        Socket listener_socket_outside_tcp( sock_tcp );
        listener_socket_outside_tcp.bind( Address( egress_addr, "0", addr_tcp ) );

        /* store port outside listener socket bound to so inside socket can connect to it */
        string listener_outside_port_tcp = to_string( listener_socket_outside_tcp.local_addr().port() );

        /* address of outside dns server for TCP requests */
        Address connect_addr_outside_tcp( "localhost", "domain", addr_tcp );

        listener_socket_outside_tcp.listen();

        /* Fork */
        ChildProcess container_process( [&]() {
                /* Unshare network namespace */
                if ( unshare( CLONE_NEWNET ) == -1 ) {
                    throw Exception( "unshare" );
                }

                TunDevice ingress_tun( "ingress", ingress_addr, egress_addr );

                /* bring up localhost */
                run( "ip link set dev lo up" );

                run( "route add -net default gw " + egress_addr );

                /* create inside listener socket for UDP dns requests */
                Socket listener_socket_inside( sock_udp );
                listener_socket_inside.bind( Address( "localhost", "domain", addr_udp ) );

                /* outside address to send UDP dns requests to */
                Address connect_addr_inside( egress_addr, listener_outside_port, addr_udp );

                /* create inside listener socket for TCP dns requests */
                Socket listener_socket_inside_tcp( sock_tcp );
                listener_socket_inside_tcp.bind( Address( "localhost", "domain", addr_tcp ) );
                listener_socket_inside_tcp.listen();

                /* outside address to send TCP dns requests to */
                Address connect_addr_inside_tcp( egress_addr, listener_outside_port_tcp, addr_tcp );

                /* Fork again */
                ChildProcess bash_process( []() {
                        const string shell = shell_path();
                        if ( execl( shell.c_str(), shell.c_str(), static_cast<char *>( nullptr ) ) < 0 ) {
                            throw Exception( "execl" );
                        }
                        return EXIT_FAILURE;
                    } );

                return ferry( ingress_tun.fd(), egress_socket, listener_socket_inside, connect_addr_inside, listener_socket_inside_tcp, connect_addr_inside_tcp, bash_process, delay_ms );
            } );

        /* set up NAT between egress and eth0 */
        NAT nat_rule;

        return ferry( egress_tun.fd(), ingress_socket, listener_socket_outside, connect_addr_outside, listener_socket_outside_tcp, connect_addr_outside_tcp, container_process, delay_ms );
    } catch ( const Exception & e ) {
        e.perror();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
