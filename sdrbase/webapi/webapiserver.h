///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2017 Edouard Griffiths, F4EXB.                                  //
//                                                                               //
// Swagger server adapter interface                                              //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#ifndef SDRBASE_WEBAPI_WEBAPISERVER_H_
#define SDRBASE_WEBAPI_WEBAPISERVER_H_

namespace qtwebapp
{
    class HttpListener;
    class HttpListenerSettings;
}

class WebAPIRequestMapper;

class WebAPIServer
{
public:
    WebAPIServer(WebAPIRequestMapper *requestMapper);
    ~WebAPIServer();

    void start();
    void stop();

private:
    WebAPIRequestMapper *m_requestMapper;
    qtwebapp::HttpListener *m_listener;
    qtwebapp::HttpListenerSettings m_settings;
};



#endif /* SDRBASE_WEBAPI_WEBAPISERVER_H_ */
