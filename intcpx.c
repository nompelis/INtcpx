/******************************************************************************
 "INtcpx" is a utility software that others may use for learning.
 On a Linux system with the GNU compiler, compile with:
 'gcc -D_DEBUG_ -D_DEBUG_DUMP_ intcpx.c -lpthread'
 You can almost do as you please with this software, subject to my terms!
 You are supposed to send chocolate if you find it useful.
 IN <nompelis@nobelware.com>
 ******************************************************************************/

/******************************************************************************
 Copyright (c) 2022-2025, Ioannis Nompelis
 All rights reserved.

 Redistribution and use in source and binary forms, with or without any
 modification, are permitted provided that the following conditions are met:
 1. Redistribution of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistribution in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 3. All advertising materials mentioning features or use of this software
    must display the following acknowledgement:
    "This product includes software developed by Ioannis Nompelis."
 4. Neither the name of Ioannis Nompelis and his partners/affiliates nor the
    names of other contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.
 5. Redistribution or use of source code and binary forms for profit must
    have written permission of the copyright holder.
 
 THIS SOFTWARE IS PROVIDED BY IOANNIS NOMPELIS ''AS IS'' AND ANY
 EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL IOANNIS NOMPELIS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#ifdef _OLDER_LINUX_
#include <fcntl.h>
#endif


struct inPthread_s {
   pthread_t tid;
   pthread_attr_t tattr;
   int inet_socket;
   struct sockaddr_in client_address;
   int (*function)(void*);
};


int daemon_start( int *inet_socket_, struct sockaddr_in *socket_name,
                  int port, int backlog )
{
   int iret,inet_socket;
   in_port_t inet_port = htons( port );;

#ifdef _DEBUG_
   printf(" [DEBUG]  Creating INET listening connection socket \n");
#endif
   inet_socket = socket( AF_INET, SOCK_STREAM, 0 );
   if( inet_socket == -1 ) {
      perror("create inet socket");
      return 1;
   }

#ifdef _OLDER_LINUX_
   int flags = fcntl( inet_socket, F_GETFL, 0 );
   if( flags == -1 ) {
      perror("fcntl(F_GETFL)");
      exit(1);
   }
   if( fcntl( inet_socket, F_SETFL, flags | O_NONBLOCK ) == -1) {
      perror("fcntl(F_SETFL)");
      exit(1);
   }
#else
   int optval=1;
   setsockopt( inet_socket, SOL_SOCKET, SO_REUSEPORT,
               &optval, sizeof(optval) );
#endif

   memset( socket_name, 0, sizeof(struct sockaddr_in) );
   socket_name->sin_family = AF_INET;
   socket_name->sin_port   = inet_port;
   iret = bind( inet_socket,
                (const struct sockaddr *) socket_name, sizeof(struct sockaddr_in) );
#ifdef _DEBUG_
   printf(" [DEBUG]  Return from bind() is \"%d\" \n", iret);
#endif
   if( iret != 0 ) {
      perror("bind inet socket");
      printf("The port may be taken?\n");
      return 2;
   }

   iret = listen( inet_socket, backlog );
#ifdef _DEBUG_
   printf(" [DEBUG]  Return from (backlog: %d) listen() is \"%d\" \n", backlog, iret);
#endif
   if( iret == -1 ) {
      perror("listen() on inet socket");
      printf("Could not listen() on the inet socket!\n");
      return 3;
   }

   *inet_socket_ = inet_socket;

   return 0;
}


void *client_thread( void *arg )
{
   struct inPthread_s *p = (struct inPthread_s *) arg;

   p->function( arg );

   free( p );
#ifdef _DEBUG_
   printf(" [DEBUG:THREAD]  Returning \n" );
#endif
   return NULL;
}


int client_spawn( int inet_socket, struct sockaddr_in client_address,
                  int (*func)(void *) )
{
   struct inPthread_s *p;
   int iret;

   p = (struct inPthread_s*) malloc( sizeof(struct inPthread_s) );
   if( p == NULL ) {
      printf(" [ERROR]  Could not allocate thread data \n");
      return -1;
   }

   memcpy( &(p->client_address), &client_address, sizeof(struct sockaddr_in) );
   p->inet_socket = inet_socket;
   p->function = func;

   iret = pthread_attr_init( &( p->tattr ) );
   if( iret != 0 ) {
      perror("creating pthread attributes");
      free( p );
      return 1;
   }

   iret = pthread_create( &( p->tid ), &( p->tattr ),
                          &client_thread, (void *) p );
   if( iret != 0 ) {
      perror("spawning pthread to serve client");
      pthread_attr_destroy( &( p->tattr ) );
      free( p );
      return 2;
   }

   pthread_detach( p->tid );
#ifdef _DEBUG_
   printf(" [DEBUG]  Spawned thread \n" );
#endif

   return 0;
}


int daemon_mainloop( int *inet_socket_, struct sockaddr_in *socket_name,
                     int (*func)(void *) )
{
   fd_set rfds;
   struct timeval tv;
   int inet_socket, nfds, isel;
   int inet_new_socket;
   struct sockaddr_in client_address;
   socklen_t client_address_size = sizeof(struct sockaddr_in);

   if( inet_socket_ == NULL || socket_name == NULL ) return 1;

   inet_socket = *inet_socket_;

#ifdef _DEBUG_
   printf(" [DEBUG]  Starting daemon main loop\n");
#endif
   while( 1 ) {
      // adding socket descriptor to the set to monitor with select
      FD_ZERO( &rfds );
      FD_SET( inet_socket, &rfds );
      nfds = inet_socket + 1;

      // setting timeout values for select
      tv.tv_sec = 1;
      tv.tv_usec = 0;

      isel = select( nfds, &rfds, NULL, NULL, &tv );
#ifdef _DEBUG_MAIN_
      printf(" [DEBUG]  Return from select() in MainLoop is %d \n", isel );
#endif
      if( isel > 0 ) {
         int k;

         // loop to possibly trap a number of descriptors
         for(k=0;k<nfds;++k) {

            if( k == inet_socket )
            if( FD_ISSET( inet_socket, &rfds ) ) {
#ifdef _DEBUG_
               printf(" [DEBUG]  Trapped inet socket (read) activity (coonection)\n");
#endif
               inet_new_socket = accept( inet_socket,
                                         (struct sockaddr *) &client_address,
                                         &client_address_size);
               if( inet_new_socket == -1 ) {
                  printf(" [ERROR]  Error connecting client to daemon \n" );
               } else {
                  if( client_spawn( inet_new_socket, client_address, func ) ) {
                     printf(" [ERROR]  Unable to spawn. Dropping client. \n");
                     shutdown( inet_new_socket, SHUT_RDWR );
                     close( inet_new_socket );
                  } else {
                     // spawning was successful; maybe we log this in a log...
                  }
               }

            }   // daemon socket conditional

         }   // trapped descriptors loop
      }

#ifdef _DEBUG_MAIN_
      printf(" [DEBUG]  Main thread (\"MainLoop\") looped...\n");
#endif
   }
#ifdef _DEBUG_
   printf(" [DEBUG]  Ending daemon main loop\n");
#endif

   return 0;
}


//
// Function to act as a demo
//

int function_demo( void *arg )
{
#ifdef _DEBUG_
   printf(" [FUNCTION]  Executing \n" );
#endif

   int i=2;
   while( i-- > 0 ) {
      sleep(3);
#ifdef _DEBUG_
      printf(" [FUNCTION]  Cycled (i= %d) \n",i );
#endif
   }

   return 0;
}


//
// A connection forwarder (like a proxy)
//

int connection_forwarder( void *arg )
{
   char hostname[] = "nobelware.com";       // HOST TO CONNECT TO
   int iport = 2222;                        // PORT TO CONNECT TO
   struct inPthread_s *p = (struct inPthread_s *) arg;
   struct hostent *host;
   struct sockaddr_in addr;
   int iret, isel, conn, client_socket, nfds, k;

   if( arg == NULL ) {
      fprintf( stdout, " [ERROR]  Pointer to payload was null \n" );
      return 1;
   }
   client_socket = p->inet_socket;

   host = gethostbyname( hostname );
   if( host == NULL ) {
      fprintf( stdout, " [ERROR]  Could not get host structure \n" );
      perror(hostname);
      return -1;
   } else {
#ifdef _DEBUG_
      fprintf( stdout, " [DEBUG]  Prepared hostname structure \n" );
#endif
   }

#ifdef _OLDER_LINUX_
   conn = socket( PF_INET, SOCK_STREAM, 0 );
   int flags = fcntl( conn, F_GETFL, 0 );
   if( flags == -1 ) {
      perror("fcntl(F_GETFL)");
      exit(1);
   }
   if( fcntl( conn, F_SETFL, flags | O_NONBLOCK ) == -1) {
      perror("fcntl(F_SETFL)");
      exit(1);
   }
#else
   conn = socket( PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0 );
#endif
   if( conn == -1 ) {
      fprintf( stdout, " [ERROR]  Could not create socket \n" );
      perror("socket creation failed");
   } else {
#ifdef _DEBUG_
      fprintf( stdout, " [DEBUG]  Created INET socket \n" );
#endif
   }

   bzero( &addr, sizeof(addr) );
   addr.sin_family = AF_INET;
   addr.sin_port = htons( iport );
   addr.sin_addr.s_addr = *(long*)(host->h_addr);

   fd_set rfds;
   struct timeval tv;

   FD_ZERO( &rfds );
   FD_SET( conn, &rfds );
   tv.tv_sec = 10;     // wait up to 10 seconds for connection
   tv.tv_usec = 0;

   iret = connect( conn, (struct sockaddr *) &addr, sizeof(addr) );
   if( iret != 0 ) {
      if( errno == EINPROGRESS ) {
#ifdef _DEBUG_
         fprintf( stdout, " [DEBUG]  Socket \"in progress\" \n " );
         perror(hostname);
#endif
         // check for writeability
         isel = select( conn+1, NULL, &rfds, NULL, &tv );
         if( isel < 0 ) {
            fprintf( stdout, " [ERROR]  Could not connect to server \n" );
            perror(hostname);
            return -2;
         } else if( isel == 0 ) {
            fprintf( stdout, " [ERROR]  Timetout connect to server \n" );
            perror(hostname);
            return -2;
         } else {
#ifdef _DEBUG_
            fprintf( stdout, " [DEBUG]  Connected (NonBlk) to server \n" );
#endif
         }
      } else {
         fprintf( stdout, " [ERROR]  Unknown socket error \n" );
         perror(hostname);
      }
   } else {
      // this should NEVER happen
      fprintf( stdout, " [ERROR]  Connection established (no drama?)\n" );
      fprintf( stdout, "  **** Something is wrong here... **** \n" );
      fprintf( stdout, "  Call to connect() should return error first, \n" );
      fprintf( stdout, "  and we should check descriptor for progress. \n" );
   }


#ifdef _DEBUG_
   fprintf( stdout, " [DEBUG]  Setting up forwarding loop \n" );
#endif
   if( conn < client_socket ) {
      nfds = client_socket + 1;
   } else {
      nfds = conn + 1;
   }

   #define SIZE 1024
   unsigned char buffer[1024]; // MODIFY THIS; I DO NOT CARE
   memset( buffer, 0, SIZE );

   iret = 0;
   while( iret == 0 ) {

      FD_ZERO( &rfds );
      FD_SET( conn, &rfds );
      FD_SET( client_socket, &rfds );

      tv.tv_sec =  1;     // cycle every 1 second
      tv.tv_usec = 0;

      isel = select( nfds, &rfds, NULL, NULL, &tv );
      if( isel > 0 ) {
         for(k=0;k<nfds;++k) {
            if( FD_ISSET( k, &rfds ) ) {
#ifdef _DEBUG_
fprintf( stdout, " [DEBUG]  Got FD with activity: %d (%d)\n", k, isel );
#endif
               if( k == conn ) {
                  int bytes = read( k, buffer, SIZE );
                  if( bytes <= 0 ) {
#ifdef _DEBUG_
fprintf( stdout, " [DEBUG]  Problem reading from server \n");
#endif
                     iret = 1;
                  } else {
                     // we reall need to throttle this
                     bytes = write( client_socket, buffer, bytes );
#ifdef _DEBUG_DUMP_
fprintf( stdout, " [DEBUG]  Got %d bytes from server: \n",bytes);
{ int i; for(i=0;i<bytes;++i) {
printf(" %.2x", (unsigned int) buffer[i] );
if( (i+1) %  8 == 0 ) printf("  ");
if( (i+1) % 16 == 0 ) printf("\n");
} printf("\n");}
#endif
                     // we reall need to check return value
                  }
               }

               if( k == client_socket ) {
                  int bytes = read( k, buffer, SIZE );
                  if( bytes <= 0 ) {
#ifdef _DEBUG_
fprintf( stdout, " [DEBUG]  Problem reading from client \n");
#endif
                     iret = 1;
                  } else {
                     // we reall need to throttle this
                     bytes = write( conn, buffer, bytes );
                     // we reall need to check return value
#ifdef _DEBUG_DUMP_
fprintf( stdout, " [DEBUG]  Got %d bytes from client: \n",bytes);
{ int i; for(i=0;i<bytes;++i) {
printf(" %.2x", (unsigned int) buffer[i] );
if( (i+1) %  8 == 0 ) printf("  ");
if( (i+1) % 16 == 0 ) printf("\n");
} printf("\n");}
#endif
                  }
               }

            }
         }
      } else {

      }
#ifdef _DEBUG_
      fprintf( stdout, " [DEBUG]  Cycled select() at %ld sec \n", tv.tv_sec );
#endif
   }

#ifdef _DEBUG_
   fprintf( stdout, " [DEBUG]  Shutting down (forwarded) connections \n" );
#endif
   shutdown( conn, SHUT_RDWR );
   close( conn );
   shutdown( client_socket, SHUT_RDWR );
   close( client_socket );

   return 0;
}


int main( int argc, char *argv[] )
{
   struct sockaddr_in socket_name;
   int inet_socket, backlog = 50, port = 4444;
   int (*function)(void*);

   function = &function_demo;          // Just show yourself that it works...
   function = &connection_forwarder;   // Then take the plunge to Hell.

   // starting the listening daemon
   if( daemon_start( &inet_socket, &socket_name, port, backlog ) ) {
      return 1;
   }

   // keep serving connections that arrive by spawning a thread per connection
   daemon_mainloop( &inet_socket, &socket_name, function );
   // just CTRL-C to terminate everything

   return 0;
}


