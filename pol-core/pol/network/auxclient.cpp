/*
History
=======
2005/11/26 Shinigami: changed "strcmp" into "stricmp" to suppress Script Errors

Notes
=======

*/

#include "auxclient.h"

#include "../../bscript/bobject.h"
#include "../../bscript/berror.h"
#include "../../bscript/bstruct.h"
#include "../../bscript/impstr.h"

#include "../../clib/cfgelem.h"
#include "../../clib/esignal.h"
#include "../../clib/sckutil.h"
#include "../../clib/socketsvc.h"
#include "../../clib/stlutil.h"
#include "../../clib/strutil.h"
#include "../../clib/threadhelp.h"
#include "../../clib/weakptr.h"
#include "../../clib/logfacility.h"

#include "../../plib/pkg.h"
#include "../polsem.h"
#include "../scrsched.h"
#include "../sockets.h"
#include "../module/uomod.h"
#include "../globals/network.h"

#include <memory>

#ifdef _MSC_VER
#pragma warning(disable:4996) // stricmp deprecation
#endif

namespace Pol {
  namespace Network {
	Bscript::BObjectImp* AuxConnection::copy() const
	{
	  return const_cast<AuxConnection*>( this );
	}

	std::string AuxConnection::getStringRep() const
	{
	  return "<AuxConnection>";
	}

	size_t AuxConnection::sizeEstimate() const
	{
      return sizeof(AuxConnection)+_ip.capacity();
	}

	bool AuxConnection::isTrue() const
	{
	  return ( _auxclientthread != NULL );
	}

	Bscript::BObjectRef AuxConnection::get_member( const char *membername )
	{
	  if ( stricmp( membername, "ip" ) == 0 )
	  {
		return Bscript::BObjectRef( new Bscript::String( _ip ) );
	  }
	  return Bscript::BObjectRef( Bscript::UninitObject::create( ) );
	}

	Bscript::BObjectImp* AuxConnection::call_method( const char* methodname, Bscript::Executor& ex )
	{
	  if ( stricmp( methodname, "transmit" ) == 0 )
	  {
		if ( ex.numParams() == 1 )
		{
		  if ( _auxclientthread != NULL )
		  {
			Bscript::BObjectImp* value = ex.getParamImp( 0 );
			// FIXME this can block!
			_auxclientthread->transmit( value );
		  }
		  else
		  {
			return new Bscript::BError( "Client has disconnected" );
		  }
		}
		else
		{
		  return new Bscript::BError( "1 parameter expected" );
		}
	  }
	  return NULL;
	}

	void AuxConnection::disconnect()
	{
	  _auxclientthread = NULL;
	}

	AuxClientThread::AuxClientThread( AuxService* auxsvc, Clib::SocketListener& listener ) :
	  SocketClientThread( listener ),
	  _auxservice( auxsvc ),
	  _uoexec( 0 )
	{}
	AuxClientThread::AuxClientThread( Core::ScriptDef scriptdef, Clib::Socket& sock ) :
	  SocketClientThread( sock ),
	  _auxservice( 0 ),
	  _scriptdef( scriptdef ),
	  _uoexec( 0 )
	{}

	bool AuxClientThread::init()
	{
      Core::PolLock lock;
	  struct sockaddr ConnectingIP = _sck.peer_address();
	  if ( ipAllowed( ConnectingIP ) )
	  {
		_auxconnection.set( new AuxConnection( this, _sck.getpeername() ) );
		Module::UOExecutorModule* uoemod;
		if ( _auxservice )
		  uoemod = Core::start_script( _auxservice->scriptdef(), _auxconnection.get() );
		else
		  uoemod = Core::start_script( _scriptdef, _auxconnection.get() );
		_uoexec = uoemod->uoexec.weakptr;
		return true;
	  }
	  else
	  {
		return false;
	  }
	}

	bool AuxClientThread::ipAllowed( sockaddr MyPeer )
	{
	  if ( !_auxservice || _auxservice->_aux_ip_match.empty() )
	  {
		return true;
	  }
	  for ( unsigned j = 0; j < _auxservice->_aux_ip_match.size(); ++j )
	  {
		unsigned int addr1part, addr2part;
		struct sockaddr_in* sockin = reinterpret_cast<struct sockaddr_in*>( &MyPeer );

		addr1part = _auxservice->_aux_ip_match[j] & _auxservice->_aux_ip_match_mask[j];
#ifdef _WIN32
		addr2part = sockin->sin_addr.S_un.S_addr & _auxservice->_aux_ip_match_mask[j];
#else
		addr2part = sockin->sin_addr.s_addr      & _auxservice->_aux_ip_match_mask[j];
#endif
		if ( addr1part == addr2part )
		  return true;
	  }
	  return false;
	}
	void AuxClientThread::run()
	{
	  if ( !init() )
	  {
		if ( _sck.connected() )
		{
		  writeline( _sck, "Connection closed" );
		  _sck.close();
		}
		_auxconnection.clear();
		return;
	  }

	  std::string tmp;
	  bool result, timeout_exit;
	  for ( ;; )
	  {
		result = readline( _sck, tmp, &timeout_exit, 5 );
        if ( !result && !timeout_exit )
          break;

		Core::PolLock lock;

		if ( _uoexec.exists() )
		{
		  if ( result )
		  {
            std::istringstream is(tmp);
			std::unique_ptr<Bscript::BObjectImp> value( _uoexec->auxsvc_assume_string ? new Bscript::String( tmp ) : Bscript::BObjectImp::unpack( is ) );

			std::unique_ptr<Bscript::BStruct> event( new Bscript::BStruct );
			event->addMember( "type", new Bscript::String( "recv" ) );
			event->addMember( "value", value.release() );
			_uoexec->os_module->signal_event( event.release() );
		  }
		}
		else
		{   // the controlling script dropped its last reference to the connection,
		  // by exiting or otherwise.
		  break;
		}
	  }

	  Core::PolLock lock;
	  _auxconnection->disconnect();
	  // the auxconnection is probably referenced by another ref_ptr, 
	  // so its deletion must be protected by the lock.  
	  // Clear our reference:
	  _auxconnection.clear();
	}

	void AuxClientThread::transmit( const Bscript::BObjectImp* value )
	{
      std::string tmp = _uoexec->auxsvc_assume_string ? value->getStringRep() : value->pack();
	  writeline( _sck, tmp );
	}

	AuxService::AuxService( const Plib::Package* pkg, Clib::ConfigElem& elem ) :
	  _pkg( pkg ),
	  _scriptdef( elem.remove_string( "SCRIPT" ), _pkg ),
	  _port( elem.remove_ushort( "PORT" ) )
	{
      std::string iptext;
	  while ( elem.remove_prop( "IPMATCH", &iptext ) )
	  {
        auto delim = iptext.find_first_of("/");
        if (delim != std::string::npos)
		{
          std::string ipaddr_str = iptext.substr(0, delim);
          std::string ipmask_str = iptext.substr(delim + 1);
		  unsigned int ipaddr = inet_addr( ipaddr_str.c_str() );
		  unsigned int ipmask = inet_addr( ipmask_str.c_str() );
		  _aux_ip_match.push_back( ipaddr );
		  _aux_ip_match_mask.push_back( ipmask );
		}
		else
		{
		  unsigned int ipaddr = inet_addr( iptext.c_str() );
		  _aux_ip_match.push_back( ipaddr );
		  _aux_ip_match_mask.push_back( 0xFFffFFffLu );
		}
	  }
	}

	void AuxService::run()
	{
      INFO_PRINT << "Starting Aux Listener (" << _scriptdef.relativename() << ", port " << _port << ")\n";

	  Clib::SocketListener listener( _port );
	  while ( !Clib::exit_signalled )
	  {
		if ( listener.GetConnection( 5 ) )
		{
          Core::PolLock lock;
		  // Shinigami: Just 4 Debugging. We got Crashes here...
#ifdef PERGON
		  ERROR_PRINT << "Aux Listener (" << _scriptdef.relativename() << ", port " << _port << ") - add task\n";
          AuxClientThread* client( new AuxClientThread( this, listener ) );
          Core::networkManager.auxthreadpool->push( [client]()
          {
            std::unique_ptr<AuxClientThread> _clientptr( client );
            _clientptr->run();
          } );
          ERROR_PRINT << "AuxWorkerSize: " << Core::networkManager.auxthreadpool->threadpoolsize() << "\n";
#else
		  Clib::SocketClientThread* clientthread = new AuxClientThread( this, listener );
		  clientthread->start();
		  // note SocketClientThread::start deletes the SocketClientThread upon thread exit.
#endif
		}
	  }
	}

	void aux_service_thread_stub( void* arg )
	{
	  AuxService* as = static_cast<AuxService*>( arg );
	  as->run();
	}

	void start_aux_services()
	{
	  for ( unsigned i = 0; i < Core::networkManager.auxservices.size(); ++i )
	  {
		threadhelp::start_thread( aux_service_thread_stub, "AuxService", Core::networkManager.auxservices[i] );
	  }
	}

	void load_auxservice_entry( const Plib::Package* pkg, Clib::ConfigElem& elem )
	{
	  Core::networkManager.auxservices.push_back( new AuxService( pkg, elem ) );
	}

	void load_aux_services()
	{
	  load_packaged_cfgs( "auxsvc.cfg", "AuxService", load_auxservice_entry );
	}
  }
}