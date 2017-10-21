/*
 * This file is part of librestd.
 *
 * Copyleft of Simone Margaritelli aka evilsocket <evilsocket@protonmail.com>
 *
 * librestd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * librestd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with librestd.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "http_server.h"
#include "log.h"

#include <string.h>
#include <regex>

namespace restd {

const static std::regex NAMED_PARAM_PARSER( ":([_a-z0-9]+)\\(([^\\)]*)\\)", std::regex_constants::icase );

http_route::http_route( string path, http_controller *controller, http_controller::handler_t handler, Method method /* = ANY */ ) :
  is_re(false),
  method(method),
  path(path),
  controller(controller),
  handler(handler){

  string tmp(path);
  std::smatch m;
  while( std::regex_search( tmp, m, NAMED_PARAM_PARSER ) == true && m.size() == 3 ) {
    is_re = true;

    string tok  = m[0].str(),
           name = m[1].str(),
           expr = m[2].str();

    strings::replace( this->path, tok, "(" + expr + ")" );

    names.push_back(name);

    log( DEBUG, "Found named parameter in '%s':", tmp.c_str() );
    for( int i = 0; i < m.size(); ++i ) {
      log( DEBUG, "  m[%d] = '%s'", i, m[i].str().c_str() );
    }

    tmp = m.suffix();
  }

  if(is_re) {
    log( DEBUG, "Final path = '%s'", this->path.c_str() );
    re = std::regex( this->path, std::regex_constants::icase );
  }
}

bool http_route::matches( http_request& req ) {
  if( method != ANY && req.method != method ) {
    return false;
  }

  if( is_re == false ) {
    return req.path == path;
  }

  std::smatch m;
  if( std::regex_search( req.path, m, re ) == true && m.size() == names.size() + 1 ) {
    
    for( int i = 1; i < m.size(); ++i ) {
      req.parameters[ names[i - 1] ] = m[i].str();
    }

    return true;
  }

  return false;
}

void http_route::call( http_request& req, http_response& resp ) {
  (controller->*handler)( req, resp );
}

void http_consumer::route( http_request& request, http_response& response ) {
  for( auto i = _routes->begin(), e = _routes->end(); i != e; ++i ){
    http_route *route = *i;

    if( route->matches( request ) ) {
      log( DEBUG, "'%s %s' matched controller %s", request.method_name().c_str(), request.path.c_str(), typeid(*route->controller).name() );
      route->call( request, response );
      return;
    }
  }

  log( WARNING, "No route defined for '%s %s'", request.method_name().c_str(), request.path.c_str() );
  
  response.not_found();
}

void http_consumer::consume( tcp_stream *client ) {
  unsigned char req_buffer[ http_request::max_size ] = {0};
  string        res_buffer;
  http_request  request;
  http_response response;
  
  log( DEBUG, "New client connection from %s:%d", client->peer_address().c_str(), client->peer_port() );

  int read = client->receive( req_buffer, http_request::max_size );
  if( read <= 0 ){
    log( ERROR, "Failed to read request from client: %d", read );
    return;
  }

  log( DEBUG, "Read %d bytes of request from client.", read );
  
  // std::regex_error might be thrown
  try {
    if( http_request::parse( request, req_buffer, read ) == false ){
      log( ERROR, "Could not parse request:\n%s", req_buffer );
      return;
    }
  }
  catch( const std::regex_error& e ){
    log( ERROR, "Exception (%d) while parsing request: %s", e.code(), e.what() );
    return;
  }
 
  route( request, response );
  
  res_buffer = response.str();
  int sent = client->send( (unsigned char *)res_buffer.c_str(), res_buffer.size() );
  if( sent != res_buffer.size() ){
    log( ERROR, "Could not send whole response, sent %d out of %lu bytes.", sent, res_buffer.size() );
  }
}

http_server::http_server( string address, unsigned short port, unsigned int threads ) :
   _address(address), _port(port), _threads(threads)
{
  for( unsigned int i = 0; i < threads; ++i ){
    _consumers.push_back( new http_consumer(_queue, &_routes) );
  }

  _server = new tcp_server( port, address.c_str() );
}

http_server::~http_server() {
  log( INFO, "Stopping http_server ..." );

  _server->stop();
  delete _server;

  for( auto i = _consumers.begin(), e = _consumers.end(); i != e; ++i ){
    delete (*i);
  }

  _consumers.clear();

  for( auto i = _routes.begin(), e = _routes.end(); i != e; ++i ){
    delete (*i);
  }

  _routes.clear();
}

void http_server::route( string path, http_controller *controller, http_controller::handler_t handler, Method method /* = ANY */ ) {
  log( DEBUG, "Registering controller %s for path '%s'", typeid(*controller).name(), path.c_str() );
  _routes.push_back( new http_route( path, controller, handler, method ) );
}

void http_server::start() {
  log( INFO, "Starting http_server ..." );
  for( auto i = _consumers.begin(), e = _consumers.end(); i != e; ++i ){
    (*i)->start();
  }

  if( _server->start() == true ){
    log( INFO, "Server listening on %s:%d with %lu workers ...", _address.c_str(), _port, _threads );
    while(1) {
      tcp_stream *client = _server->accept();
      if( client ){
        _queue.add(client);
      }
    }
  }
}

}
