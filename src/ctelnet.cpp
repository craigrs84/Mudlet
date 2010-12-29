/***************************************************************************
 *   copyright (C) 2002-2005 by Tomas Mecir (kmuddy@kmuddy.com)
 *   copyright (C) 2008-2009 by Heiko Koehn (KoehnHeiko@googlemail.com
 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "ctelnet.h"
#include <time.h>
#include <unistd.h>
#include <QTextCodec>
#include <QHostAddress>
#include <iostream>
#include <string>
#include <sstream>
#include <sys/types.h>
#include <stdio.h>
#include <QDebug>
#include <QDir>
#include <QTcpSocket>
#include "mudlet.h"
#include "TDebug.h"
#include "dlgComposer.h"

#ifdef DEBUG
#undef DEBUG
#endif

//#define DEBUG

extern QStringList gSysErrors;

using namespace std;



cTelnet::cTelnet( Host * pH )
: mResponseProcessed( true )
, mAlertOnNewData( true )
, mGA_Driver( false )
, mFORCE_GA_OFF( false )
, mpHost(pH)
, mpPostingTimer( new QTimer( this ) )
, mUSE_IRE_DRIVER_BUGFIX( false )
, mLF_ON_GA( false )

, mCommands( 0 )
, mMCCP_version_1( false )
, mMCCP_version_2( false )
, enableATCP( false )
, enableGMCP( false )

, enableChannel102( false )
, mpComposer( 0 )

{

    mIsTimerPosting = false;
    mNeedDecompression = false;
    mWaitingForCompressedStreamToStart = false;
    // initialize default encoding
    encoding = "UTF-8";
    encodingChanged(encoding);
    termType = "Mudlet 1.1";
    iac = iac2 = insb = false;
    command = "";
    curX = 80;
    curY = 25;

    // initialize the socket
    connect(&socket, SIGNAL(connected()), this, SLOT(handle_socket_signal_connected()));
    connect(&socket, SIGNAL(disconnected()), this, SLOT(handle_socket_signal_disconnected()));
    //connect(&socket, SIGNAL(error()), this, SLOT (handle_socket_signal_error()));
    connect(&socket, SIGNAL(readyRead()), this, SLOT (handle_socket_signal_readyRead()));
    //connect(&socket, SIGNAL(hostFound()), this, SLOT (handle_socket_signal_hostFound()));

    // initialize telnet session
    reset ();

    mpPostingTimer->setInterval( 100 );//FIXME
    connect(mpPostingTimer, SIGNAL(timeout()), this, SLOT(slot_timerPosting()));

    mTimerLogin = new QTimer( this );
    mTimerLogin->setSingleShot(true);
    connect(mTimerLogin, SIGNAL(timeout()), this, SLOT(slot_send_login()));

    mTimerPass = new QTimer( this );
    mTimerPass->setSingleShot( true );
    connect(mTimerPass, SIGNAL(timeout()), this, SLOT(slot_send_pass()));
}

void cTelnet::reset ()
{
    //prepare option variables
    for (int i = 0; i < 256; i++)
    {
        myOptionState[i] = false;
        hisOptionState[i] = false;
        announcedState[i] = false;
        heAnnouncedState[i] = false;
        triedToEnable[i] = false;
    }
    iac = iac2 = insb = false;
    command = "";
}


cTelnet::~cTelnet()
{
    disconnect();
    socket.deleteLater();
}


void cTelnet::encodingChanged(QString encoding)
{
    //cout << "cTelnet::encodingChanged() called!" << endl;
    encoding = encoding;

    // unicode carries information in form of single byte characters
    // and multiple byte character sequences.
    // the encoder and the decoder maintain translation state, i.e. they need to know the preceding
    // chars to make the correct decisions when translating into unicode and vice versa

    incomingDataCodec = QTextCodec::codecForName(encoding.toLatin1().data());
    incomingDataDecoder = incomingDataCodec->makeDecoder();

    outgoingDataCodec = QTextCodec::codecForName(encoding.toLatin1().data());
    outgoingDataDecoder = outgoingDataCodec->makeEncoder();
}


void cTelnet::connectIt(const QString &address, int port)
{
    // wird an dieser Stelle gesetzt
    if( mpHost )
    {
        mUSE_IRE_DRIVER_BUGFIX = mpHost->mUSE_IRE_DRIVER_BUGFIX;
        mLF_ON_GA = mpHost->mLF_ON_GA;
        mFORCE_GA_OFF = mpHost->mFORCE_GA_OFF;
    }
    if( socket.state() != QAbstractSocket::UnconnectedState )
    {
        socket.abort();
        connectIt( address, port );
        return;

    }
    hostName = address;
    hostPort = port;
    #if QT_VERSION >= 0x040500
        gSysErrors.removeDuplicates();
    #endif
    for( int i=0; i<gSysErrors.size(); i++ )
    {
        QString m = gSysErrors[i];
        m.append("\n");
        if( m.indexOf("[ERROR]") != -1 )
        {
            mpHost->mpConsole->print( m, 255, 0, 0, 0, 0, 0 );
        }
        else
        {
            mpHost->mpConsole->print( m, 0, 255, 0, 0, 0, 0 );
        }
    }

    QString server = "[INFO] looking up the IP address of server:" + address + ":" + QString::number(port) + " ...\n";
    postMessage( server );
    QHostInfo::lookupHost(address, this, SLOT(handle_socket_signal_hostFound(QHostInfo)));
}


void cTelnet::disconnect ()
{
    socket.disconnectFromHost();
}

void cTelnet::handle_socket_signal_error()
{
    QString err = "[ERROR] TCP/IP socket ERROR:" + socket.errorString() + "\n";
    postMessage( err );
}

void cTelnet::slot_send_login()
{
    sendData( mpHost->getLogin() );
}

void cTelnet::slot_send_pass()
{
    sendData( mpHost->getPass() );
}

void cTelnet::handle_socket_signal_connected()
{
    reset();
    QString msg = "[INFO] A connection has been established successfully.\n";
    postMessage( msg );
    QString func = "onConnect";
    QString nothing = "";
    mpHost->mLuaInterpreter.call(func, nothing );
    mConnectionTime.start();
    if( (mpHost->getLogin().size()>0) && (mpHost->getPass().size()>0))
        mTimerLogin->start(2000);
    if( (mpHost->getPass().size()>0)  && (mpHost->getPass().size()>0))
        mTimerPass->start(3000);

    /*
    //negotiate some telnet options, if allowed
    string cmd;
    //NAWS (used to send info about window size)
    cmd += TN_IAC;
    cmd += TN_WILL;
    cmd += OPT_NAWS;
    //do not allow server to echo our text!
    cmd += TN_IAC;
    cmd += TN_DONT;
    cmd += OPT_ECHO;
    socketOutRaw(cmd);
    setDisplayDimensions();*/
}

void cTelnet::handle_socket_signal_disconnected()
{
    postData();
    QString msg;
    QTime timeDiff(0,0,0,0);
    msg = QString("[INFO] connection time: %1\n").arg(timeDiff.addMSecs(mConnectionTime.elapsed()).toString("hh:mm:ss.zzz"));
    mNeedDecompression = false;
    reset();
    QString lf = "\n\n";
    QString err = "[INFO] Socket got disconnected. " + socket.errorString() + "\n";
    QString spacer = "-------------------------------------------------------------\n";
    if( ! mpHost->mIsGoingDown )
    {
        postMessage( lf );
        postMessage( spacer );
        postMessage( err );
        postMessage( msg );
        postMessage( spacer );
    }
}

void cTelnet::handle_socket_signal_hostFound(QHostInfo hostInfo)
{
    if(!hostInfo.addresses().isEmpty())
    {
        mHostAddress = hostInfo.addresses().first();
        QString msg = "[INFO] The IP address of "+hostName+" has been found. It is: "+mHostAddress.toString()+"\n";
        postMessage( msg );
        msg = "[INFO] trying to connect to "+mHostAddress.toString()+":"+QString::number(hostPort)+" ...\n";
        postMessage( msg );
        socket.connectToHost(mHostAddress, hostPort);
    }
    else
    {
        socket.connectToHost(hostInfo.hostName(), hostPort);
        QString msg = "[ERROR] Host name lookup Failure! Connection cannot be established. The server name is not correct, not working properly, or your nameservers are not working properly.\n";
        postMessage( msg );
        return;
    }
}

bool cTelnet::sendData( QString & data )
{
    while( data.indexOf("\n") != -1 )
    {
        data.replace(QChar('\n'),"");
    }
    TEvent * pE = new TEvent;
    pE->mArgumentList.append( "sysDataSendRequest" );
    pE->mArgumentTypeList.append( ARGUMENT_TYPE_STRING );
    pE->mArgumentList.append( data );
    pE->mArgumentTypeList.append( ARGUMENT_TYPE_STRING );
    mpHost->raiseEvent( pE );
    if( mpHost->mAllowToSendCommand )
    {
        string outdata = (outgoingDataCodec->fromUnicode(data)).data();
        if( ! mpHost->mUSE_UNIX_EOL )
        {
            outdata.append("\r\n");
        }
        else
            outdata += "\n";
        return socketOutRaw( outdata );
    }
    else
    {
        mpHost->mAllowToSendCommand = true;
        return false;
    }
}


bool cTelnet::socketOutRaw(string & data)
{
    if( ! socket.isWritable() )
    {
        //qDebug()<<"MUDLET SOCKET ERROR: socket not connected, but wants to send data="<<data.c_str();
        return false;
    }
    int dataLength = data.length();
    int remlen = dataLength;

    /*cout << "SOCKET OUT RAW: [ ";
    for(unsigned int i=0;i<data.size();i++)
    {
        unsigned char c = data[i];
        int ci = 0;
        ci = (int)c;
        cout << "<" << ci << "> ";
    }
    cout << " ]" << endl;*/

    do
    {
        int written = socket.write(data.data(), remlen);

        if (written == -1)
        {
            return false;
        }
        remlen -= written;
        dataLength += written;
    }
    while(remlen > 0);

    if( mGA_Driver )
    {
        mCommands++;
        if( mCommands == 1 )
        {
            mWaitingForResponse = true;
            networkLatencyTime.restart();
        }
    }

    return true;
}



void cTelnet::setDisplayDimensions()
{
    int x = mpHost->mWrapAt;
    int y = mpHost->mScreenHeight;
    if(myOptionState[static_cast<int>(OPT_NAWS)])
    {
        //cout<<"TELNET: sending NAWS:"<<x<<"x"<<y<<endl;
        string s;
        s = TN_IAC;
        s += TN_SB;
        s += OPT_NAWS;
        char x1, x2, y1, y2;
        x1 = x / 256;
        x2 = x % 256;
        y1 = y / 256;
        y2 = y % 256;
        //IAC must be doubled
        s += x1;
        if(x1 == TN_IAC)
        s += TN_IAC;
        s += x2;
        if (x2 == TN_IAC)
        s += TN_IAC;
        s += y1;
        if (y1 == TN_IAC)
        s += TN_IAC;
        s += y2;
        if (y2 == TN_IAC)
        s += TN_IAC;

        s += TN_IAC;
        s += TN_SE;
        socketOutRaw(s);
    }
}

void cTelnet::sendTelnetOption (char type, char option)
{
    //cout << "CLIENT SENDING Telnet option: command<"<<(int)type<<"> option<"<<(int)option<<">"<<endl;
    string cmd;
    cmd += TN_IAC;
    cmd += type;
    cmd += option;
    socketOutRaw(cmd);
}



void cTelnet::processTelnetCommand( const string & command )
{
  const string ayt_response = "delay\r\n";
  char ch = command[1];
  char option;
  switch( ch )
  {
      case TN_GA:
      {
          #ifdef DEBUG
            cout << "cTelnet::processTelnetCommand() command = TN_GA"<<endl;
          #endif
          recvdGA = true;
          break;
      }
      case TN_WILL:
      {
          //server wants to enable some option (or he sends a timing-mark)...
          option = command[2];
          int idxOption = static_cast<int>(option);
          if( option == static_cast<char>(200) ) // ATCP support
          {
              //FIXME: this is a bug, some muds offer both atcp + gmcp
              if( mpHost->mEnableGMCP ) break;

              cout << "ATCP enabled" << endl;
              enableATCP = true;
              sendTelnetOption( TN_DO, 200 );

              string _h;
              _h += TN_IAC;
              _h += TN_SB;
              _h += 200;
              _h += "hello Mudlet 1.2.0\ncomposer 1\nchar_vitals 1\nroom_brief 1\nroom_exits 1\n";
              _h += TN_IAC;
              _h += TN_SE;
              socketOutRaw( _h );
              break;
          }

          if( option == GMCP )
          {
              if( !mpHost->mEnableGMCP ) break;

              enableGMCP = true;
              sendTelnetOption( TN_DO, GMCP );
              cout << "GMCP enabled" << endl;

              string _h;
              _h = TN_IAC;
              _h += TN_SB;
              _h += GMCP;
              _h += "Core.Hello { \"client\": \"Mudlet\", \"version\": \"1.2.0\" }";
              _h += TN_IAC;
              _h += TN_SE;

              socketOutRaw( _h );

              _h = TN_IAC;
              _h += TN_SB;
              _h += GMCP;
              _h += "Core.Supports.Set [ \"Char 1\", \"Char.Skills 1\", \"Char.Items 1\", \"Room 1\", \"IRE.Composer 1\"]";
              _h += TN_IAC;
              _h += TN_SE;

              socketOutRaw( _h );

              /*if ((mpHost->getLogin().size()>0) && (mpHost->getPass().size()>0)) {
                _h = TN_IAC;
                _h += TN_SB;
                _h += GMCP;
                _h += "Char.Login { \"name\": \"";
                _h += mpHost->getLogin().toLatin1().data();
                _h += "\", \"password\": \"";
                _h += mpHost->getPass().toLatin1().data();
                _h += "\" }";
                _h += TN_IAC;
                _h += TN_SE;

                cout << "  sending: " << _h << endl;
                socketOutRaw( _h );
              }*/
              break;
          }

          //option = command[2];
          if( option == static_cast<char>(102) ) // Aardwulf channel 102 support
          {
              cout << "Aardwulf channel 102 support enabled" << endl;
              enableChannel102 = true;
              sendTelnetOption( TN_DO, 102 );
              break;
          }

          heAnnouncedState[idxOption] = true;
          if( triedToEnable[idxOption] )
          {
              hisOptionState[idxOption] = true;
              triedToEnable[idxOption] = false;
          }
          else
          {
              if( !hisOptionState[idxOption] )
              {
                   //only if this is not set; if it's set, something's wrong wth the server
                   //(according to telnet specification, option announcement may not be
                   //unless explicitly requested)

                   if( ( option == OPT_SUPPRESS_GA ) ||
                       ( option == OPT_STATUS ) ||
                       ( option == OPT_TERMINAL_TYPE) ||
                       ( option == OPT_ECHO ) ||
                       ( option == OPT_NAWS ) )
                   {
                       sendTelnetOption( TN_DO, option );
                       hisOptionState[idxOption] = true;
                   }
                   else if( ( option == OPT_COMPRESS ) || ( option == OPT_COMPRESS2 ) )
                   {
                       //these are handled separately, as they're a bit special
                       if( mpHost->mFORCE_NO_COMPRESSION || ( ( option == OPT_COMPRESS ) && ( hisOptionState[static_cast<int>(OPT_COMPRESS2)] ) ) )
                       {
                           //protocol says: reject MCCP v1 if you have previously accepted
                           //MCCP v2...
                           sendTelnetOption( TN_DONT, option );
                           hisOptionState[idxOption] = false;
                           cout << "Rejecting MCCP v1, because v2 has already been negotiated." << endl;
                       }
                       else
                       {
                           sendTelnetOption( TN_DO, option );
                           hisOptionState[idxOption] = true;
                           //inform MCCP object about the change
                           if( ( option == OPT_COMPRESS ) )
                           {
                               mMCCP_version_1 = true;
                               //MCCP->setMCCP1(true);
                               cout << "MCCP v1 negotiated." << endl;
                           }
                           else
                           {
                               mMCCP_version_2 = true;
                               //MCCP->setMCCP2( true );
                               cout << "MCCP v2 negotiated!" << endl;
                           }
                       }
                   }
                   else
                   {
                       sendTelnetOption( TN_DONT, option );
                       hisOptionState[idxOption] = false;
                   }
               }
          }
          break;
      }

      case TN_WONT:
      {

          //server refuses to enable some option...
          #ifdef DEBUG
              cout << "cTelnet::processTelnetCommand() command = TN_WONT"<<endl;
          #endif
          option = command[2];
          int idxOption = static_cast<int>(option);
          if( triedToEnable[idxOption] )
          {
              hisOptionState[idxOption] = false;
              triedToEnable[idxOption] = false;
              heAnnouncedState[idxOption] = true;
          }
          else
          {
              //send DONT if needed (see RFC 854 for details)
              if( hisOptionState[idxOption] || ( heAnnouncedState[idxOption] ) )
              {
                  sendTelnetOption( TN_DONT, option );
                  hisOptionState[idxOption] = false;

                  if( ( option == OPT_COMPRESS ) )
                  {
                      //MCCP->setMCCP1 (false);
                      mMCCP_version_1 = false;
                      cout << "MCCP v1 disabled !" << endl;
                  }
                  if( ( option == OPT_COMPRESS2 ) )
                  {
                      mMCCP_version_2 = false;
                      //MCCP->setMCCP2 (false);
                      //      cout << "MCCP v1 disabled !" << endl;
                  }
              }
              heAnnouncedState[idxOption] = true;
          }
          break;
      }

      case TN_DO:
      {
          //server wants us to enable some option
          option = command[2];
          int idxOption = static_cast<int>(option);
          if( option == static_cast<char>(200) ) // ATCP support
          {
            cout << "TELNET IAC DO ATCP" << endl;
            enableATCP = true;
            sendTelnetOption( TN_WILL, 200 );
            break;
          }
          if( option == static_cast<char>(201) ) // GMCP support
          {
            cout << "TELNET IAC DO GMCP" << endl;
            enableATCP = true;
            sendTelnetOption( TN_WILL, 201 );
            break;
          }
          if( option == static_cast<char>(102) ) // channel 102 support
          {
            cout << "TELNET IAC DO CHANNEL 102" << endl;
            enableChannel102 = true;
            sendTelnetOption( TN_WILL, 102 );
            break;
          }
              //cout << "server wants us to enable telnet option " << (int)option << "(TN_DO + "<< (int)option<<")"<<endl;
          if(option == OPT_TIMING_MARK)
          {
              cout << "OK we are willing to enable TIMING_MARK" << endl;
              //send WILL TIMING_MARK
              sendTelnetOption( TN_WILL, option );
          }
          else if (!myOptionState[255])
          //only if the option is currently disabled
          {
              if( ( option == OPT_SUPPRESS_GA ) ||
                  ( option == OPT_STATUS ) ||
                  ( option == OPT_NAWS ) ||
                  ( option == OPT_TERMINAL_TYPE ) )
              {
                  if( option == OPT_SUPPRESS_GA ) cout << "OK we are willing to enable option SUPPRESS_GA"<<endl;
                  if( option == OPT_STATUS ) cout << "OK we are willing to enable telnet option STATUS"<<endl;
                  if( option == OPT_TERMINAL_TYPE ) cout << "OK we are willing to enable telnet option TERMINAL_TYPE"<<endl;
                  if( option == OPT_NAWS ) cout << "OK we are willing to enable telnet option NAWS"<<endl;
                  sendTelnetOption( TN_WILL, option );
                  myOptionState[idxOption] = true;
                  announcedState[idxOption] = true;
              }
              else
              {
                  cout << "SORRY, we are NOT WILLING to enable this telnet option." << endl;
                  sendTelnetOption (TN_WONT, option);
                  myOptionState[idxOption] = false;
                  announcedState[idxOption] = true;
              }
          }
          if( option == OPT_NAWS )
          {
                                        //NAWS
              setDisplayDimensions();
          }
          break;
      }
      case TN_DONT:
      {
          //only respond if value changed or if this option has not been announced yet
              //cout << "cTelnet::processTelnetCommand() command = TN_DONT"<<endl;
          option = command[2];
          int idxOption = static_cast<int>(option);
          if( myOptionState[idxOption] || ( !announcedState[idxOption] ) )
          {
              sendTelnetOption (TN_WONT, option);
              announcedState[idxOption] = true;
          }
          myOptionState[idxOption] = false;
          break;
      }
      case TN_SB:
      {
          //subcommand - we analyze and respond...
          option = command[2];
          //~ unsigned char c = option;
          //~ printf("SB option: %d\n", (int)c);

          /* ATCP */
          if( option == static_cast<char>(200) )
          {
              QString _m = command.c_str();
              if( command.size() < 6 ) return;
              _m = _m.mid( 3, command.size()-5 );
              setATCPVariables( _m );
              if( _m.startsWith("Auth.Request") )
              {
                  string _h;
                  _h += TN_IAC;
                  _h += TN_SB;
                  _h += 200;
                  _h += "hello Mudlet 1.0.6\ncomposer 1\nchar_vitals 1\nroom_brief 1\nroom_exits 1\n";
                  _h += TN_IAC;
                  _h += TN_SE;
                  socketOutRaw( _h );
              }

              return;
          }

          /* GMCP */
          if( option == GMCP )
          {
              QString _m = command.c_str();
              if( command.size() < 6 ) return;
              _m = _m.mid( 3, command.size()-5 );
              setGMCPVariables( _m );

              return;
          }

          if( option == static_cast<char>(102) )
          {
              QString _m = command.c_str();
              if( command.size() < 6 ) return;
              _m = _m.mid( 3, command.size()-5 );
              setChannel102Variables( _m );
              return;
          }
          switch( option ) //switch 2
          {
              case OPT_STATUS:
              {
                  //see OPT_TERMINAL_TYPE for explanation why I'm doing this
                  if( true )
                  {
                      cout << "WARNING: FIXME #501" << endl;
                      if(command[3] == TNSB_SEND)
                      {
                          cout << "WARNING: FIXME #504" << endl;
                          //request to send all enabled commands; if server sends his
                          //own list of commands, we just ignore it (well, he shouldn't
                          //send anything, as we do not request anything, but there are
                          //so many servers out there, that you can never be sure...)
                          string cmd;
                          cmd +=  TN_IAC;
                          cmd +=  TN_SB;
                          cmd +=  OPT_STATUS;
                          cmd +=  TNSB_IS;
                          for (short i = 0; i < 256; i++)
                          {
                              if (myOptionState[i])
                              {
                                  cmd +=  TN_WILL;
                                  cmd +=  i;
                              }
                              if (hisOptionState[i])
                              {
                                  cmd +=  TN_DO;
                                  cmd +=  i;
                              }
                          }
                          cmd +=  TN_IAC;
                          cmd +=  TN_SE;
                          socketOutRaw(cmd);
                      }
                  }
                  break;
              }

              case OPT_TERMINAL_TYPE:
              {
                  cout << "server sends telnet option terminal type"<<endl;
                  if( myOptionState[static_cast<int>(OPT_TERMINAL_TYPE)] )
                  {
                      if(command[3] == TNSB_SEND )
                      {
                           //server wants us to send terminal type; he can send his own type
                           //too, but we just ignore it, as we have no use for it...
                           string cmd;
                           cmd +=  TN_IAC;
                           cmd +=  TN_SB;
                           cmd +=  OPT_TERMINAL_TYPE;
                           cmd +=  TNSB_IS;
                           cmd += termType.toLatin1().data();
                           cmd +=  TN_IAC;
                           cmd +=  TN_SE;
                           socketOutRaw( cmd );
                      }
                  }
                  //other cmds should not arrive, as they were not negotiated.
                  //if they do, they are merely ignored
              }
          };//end switch 2
          //other commands are simply ignored (NOP and such, see .h file for list)
      }
  };//end switch 1
}

void cTelnet::setATCPVariables( QString & msg )
{
    QString var;
    QString arg;
    bool single = true;
    if( msg.indexOf( '\n' ) > -1 )
    {
        var = msg.section( "\n", 0, 0 );
        arg = msg.section( "\n", 1 );
        single = false;
    }
    else
    {
        var = msg.section( " ", 0, 0 );
        arg = msg.section( " ", 1 );
    }

    if( var.startsWith("Client.Compose") )
    {
        QString title;
        if( ! single )
            title = var.section( " ", 1 );
        else
        {
            title = arg;
            arg = "";
        }
        if( mpComposer )
        {
            return;
        }
        mpComposer = new dlgComposer( mpHost );
        //FIXME
        if( arg.startsWith(" ") )
        {
            arg.remove(0,1);
        }
        mpComposer->init( title, arg );
        mpComposer->raise();
        mpComposer->show();
        return;
    }
    var.remove( '.' );
    arg.remove( '\n' );
    int space = var.indexOf( ' ' );
    if( space > -1 )
    {
        arg.prepend(" ");
        arg = arg.prepend( var.section( " ", 1 ) );
        var = var.section( " ", 0, 0 );
    }
    mpHost->mLuaInterpreter.setAtcpTable( var, arg );
    if( var.startsWith("RoomNum") )
    {
        if( mpHost->mpMap )
        {
            mpHost->mpMap->mRoomId = arg.toInt();
            if( mpHost->mpMap->mpM )
            {
                mpHost->mpMap->mpM->update();
            }
        }
    }
}

void cTelnet::setGMCPVariables( QString & msg )
{
    QString var;
    QString arg;
    bool single = true;
    if( msg.indexOf( '\n' ) > -1 )
    {
        var = msg.section( "\n", 0, 0 );
        arg = msg.section( "\n", 1 );
        single = false;
    }
    else
    {
        var = msg.section( " ", 0, 0 );
        arg = msg.section( " ", 1 );
    }
    arg.remove( '\n' );
    printf("message: '%s', body: '%s'\n", var.toLatin1().data(), arg.toLatin1().data());
    mpHost->mLuaInterpreter.setGMCPTable( var, arg );
}

void cTelnet::setChannel102Variables( QString & msg )
{
    // messages consist of 2 bytes only
    if( msg.size() < 2 )
    {
        qDebug()<<"ERROR: channel 102 message size != 2 bytes msg<"<<msg<<">";
        return;
    }
    else
    {
        int _m = msg.at(0).toAscii();
        int _a = msg.at(1).toAscii();
        mpHost->mLuaInterpreter.setChannel102Table( _m, _a );
    }
}

void cTelnet::atcpComposerCancel()
{
    if( ! mpComposer ) return;
    mpComposer->close();
    mpComposer = 0;
    string msg = "*q\nno\n";
    socketOutRaw( msg );
}

void cTelnet::atcpComposerSave( QString txt )
{
    if( ! mpHost->mEnableGMCP )
    {
        //olesetbuf \n <text>
        string _h;
        _h += TN_IAC;
        _h += TN_SB;
        _h += 200;
        _h += "olesetbuf \n ";
        _h += txt.toLatin1().data();
            _h += '\n';
        _h += TN_IAC;
        _h += TN_SE;
        socketOutRaw( _h );
        _h.clear();
        _h += "*s\n";
        socketOutRaw( _h );
    }
    else
    {
        string _h;
        _h += TN_IAC;
        _h += TN_SB;
        _h += GMCP;
        _h += "IRE.Composer.SetBuffer";
        if( txt != "" )
        {
            _h += "  ";
            _h += txt.toLatin1().data();
            _h += " ";
        }
        _h += TN_IAC;
        _h += TN_SE;
        socketOutRaw( _h );
        _h.clear();
        _h += "*s\n";
        socketOutRaw( _h );
    }
    if( ! mpComposer ) return;
    mpComposer->close();
    mpComposer = 0;
}

/*string cTelnet::getCurrentTime()
{
    time_t t;
    time(&t);
    tm lt;
    ostringstream s;
    s.str("");
    struct timeval tv;
    struct timezone tz;
    gettimeofday(&tv, &tz);
    localtime_r( &t, &lt );
    s << "["<<lt.tm_hour<<":"<<lt.tm_min<<":"<<lt.tm_sec<<":"<<tv.tv_usec<<"]";
    string time = s.str();
    return time;
} */

void cTelnet::postMessage( QString msg )
{
    //mudlet::self()->printSystemMessage( mpHost, msg );
    if( ! msg.endsWith( '\n' ) )
    {
        msg.append("\n");
    }
    if( msg.indexOf("[ERROR]") != -1 )
    {
        mpHost->mpConsole->print( msg, 255, 0, 0, 0, 0, 0 );
    }
    else
    {
        mpHost->mpConsole->print( msg, 0, 255, 0, 0, 0, 0 );
    }
}

//forward data for further processing


void cTelnet::gotPrompt( string & mud_data )
{
    mpPostingTimer->stop();
    mMudData += mud_data;

    if( mUSE_IRE_DRIVER_BUGFIX && mGA_Driver )
    {
        //////////////////////////////////////////////////////////////////////
        //
        // Patch for servers that need GA/EOR for prompt fixups
        //

        int j = 0;
        int s = mMudData.size();
        while( j < s )
        {
            // search for leading <LF> but skip leading ANSI control sequences
            if( mMudData[j] == 0x1B )
            {
                while( j < s )
                {
                    if( mMudData[j] == 'm' )
                    {
                        goto NEXT;
                        break;
                    }
                    j++;
                }
            }
            if( mMudData[j] == '\n' )
            {
                mMudData.erase( j, 1 );
                break;
            }
            else
            {
                break;
            }
            NEXT: j++;
        }
        //
        ////////////////////////////
    }

    postData();
    mMudData = "";
    mIsTimerPosting = false;
}

void cTelnet::gotRest( string & mud_data )
{
    if( mud_data.size() < 1 )
    {
        return;
    }
    //if( ( ! mGA_Driver ) || ( mud_data[mud_data.size()-1] == '\n' ) )
    if( ! mGA_Driver )
    {
        size_t i = mud_data.rfind('\n');
        if( i != string::npos )
        {
            mMudData += mud_data.substr( 0, i+1 );
            postData();
            mpPostingTimer->start();
            mIsTimerPosting = true;
            if( i+1 < mud_data.size() )
            {
                mMudData = mud_data.substr( i+1, mud_data.size() );
            }
            else
            {
                mMudData = "";
            }
        }
        else
        {
            mMudData += mud_data;
            if( ! mIsTimerPosting )
            {
                mpPostingTimer->start();
                mIsTimerPosting = true;
            }
        }

    }
    else if( mGA_Driver )// if( ( mud_data[mud_data.size()-1] == '\n' ) )
    {

        //mpPostingTimer->stop();
        mMudData += mud_data;
        postData();
        mMudData = "";
        //mIsTimerPosting = false;
    }
    else
    {
        mMudData += mud_data;
        if( ! mIsTimerPosting )
        {
            mpPostingTimer->start();
            mIsTimerPosting = true;
        }
    }
}

void cTelnet::slot_timerPosting()
{
    if( ! mIsTimerPosting ) return;
    mMudData += "\r";
    postData();
    mMudData = "";
    mIsTimerPosting = false;
    mpHost->mpConsole->finalize();
}

void cTelnet::postData()
{
    //QString cd = incomingDataDecoder->toUnicode( mMudData.data(), mMudData.size() );
    mpHost->mpConsole->printOnDisplay( mMudData );
    if( mAlertOnNewData )
    {
        QApplication::alert( mudlet::self(), 0 );
    }
}

void cTelnet::initStreamDecompressor()
{
    mZstream.zalloc = Z_NULL;
    mZstream.zfree = Z_NULL;
    mZstream.opaque = Z_NULL;
    mZstream.avail_in = 0;
    mZstream.next_in = Z_NULL;

    inflateInit( & mZstream );
}

int cTelnet::decompressBuffer( char * dirtyBuffer, int length )
{
    char cleanBuffer[100001]; //clean data after decompression

    mZstream.avail_in = length;
    mZstream.next_in = (Bytef *) dirtyBuffer;

    mZstream.avail_out = 100000;
    mZstream.next_out = (Bytef *) cleanBuffer;

    int zval = inflate( & mZstream, Z_SYNC_FLUSH );
    int outSize = 100000 - mZstream.avail_out;

    if (zval == Z_STREAM_END)
    {
        inflateEnd( & mZstream );
    }
    else
    {
        if (zval < 0)
        {
            initStreamDecompressor();
            return -1;
        }
    }
    memcpy( dirtyBuffer, cleanBuffer, outSize );
    return outSize;
}


void cTelnet::recordReplay()
{
    lastTimeOffset = 0;
    timeOffset.start();
}

char loadBuffer[100001];
int loadedBytes;
QDataStream replayStream;
bool loadingReplay;
QFile replayFile;

void cTelnet::loadReplay( QString & name )
{
    replayFile.setFileName( name );
    QString msg = "loading replay " + name;
    postMessage( msg );
    replayFile.open( QIODevice::ReadOnly );
    replayStream.setDevice( &replayFile );
    loadingReplay = true;
    mudlet::self()->replayStart();
    _loadReplay();
}

void cTelnet::_loadReplay()
{
    if( ! replayStream.atEnd() )
    {
        int offset;
        int amount;
        replayStream >> offset;
        replayStream >> amount;

        char * pB = &loadBuffer[0];
        loadedBytes = replayStream.readRawData ( pB, amount );
        qDebug()<<"loaded:"<<loadedBytes<<"/"<<amount<<" bytes"<<" waiting for "<<offset<<" milliseconds";
        loadBuffer[loadedBytes+1] = '\0';
        QTimer::singleShot( offset/mudlet::self()->mReplaySpeed, this, SLOT(readPipe()));
        mudlet::self()->mReplayTime = mudlet::self()->mReplayTime.addMSecs(offset);
    }
    else
    {
        loadingReplay = false;
        replayFile.close();
        QString msg = "The replay has ended.\n";
        postMessage( msg );
        mudlet::self()->replayOver();
    }
}


void cTelnet::readPipe()
{
    int datalen = loadedBytes;
    string cleandata = "";
    recvdGA = false;
    for( int i = 0; i < datalen; i++ )
    {
        char ch = loadBuffer[i];

        if( iac || iac2 || insb || (ch == TN_IAC) )
        {
            #ifdef DEBUG
                cout <<" SERVER SENDS telnet command "<<(int)ch<<endl;
            #endif
            if (! (iac || iac2 || insb) && ( ch == TN_IAC ) )
            {
                iac = true;
                command += ch;
            }
            else if (iac && (ch == TN_IAC) && (!insb))
            {
                //2. seq. of two IACs
                iac = false;
                cleandata += ch;
                command = "";
            }
            else if( iac && ( ! insb) && ( (ch == TN_WILL) || (ch == TN_WONT) || (ch == TN_DO) || (ch == TN_DONT)))
            {
                //3. IAC DO/DONT/WILL/WONT
                iac = false;
                iac2 = true;
                command += ch;
            }
            else if( iac2 )
            {
                //4. IAC DO/DONT/WILL/WONT <command code>
                iac2 = false;
                command += ch;
                processTelnetCommand( command );
                command = "";
            }
            else if( iac && (!insb) && (ch == TN_SB))
            {
                //cout << getCurrentTime()<<" GOT TN_SB"<<endl;
                //5. IAC SB
                iac = false;
                insb = true;
                command += ch;
            }
            else if(iac && (!insb) && (ch == TN_SE))
            {
                //6. IAC SE without IAC SB - error - ignored
                command = "";
                iac = false;
            }
            else if( insb )
            {
                //7. inside IAC SB
                command += ch;
                if( iac && (ch == TN_SE))  //IAC SE - end of subcommand
                {
                    processTelnetCommand( command );
                    command = "";
                    iac = false;
                    insb = false;
                }
                if( iac ) iac = false;
                else if( ch == TN_IAC ) iac = true;
            }
            else
            //8. IAC fol. by something else than IAC, SB, SE, DO, DONT, WILL, WONT
            {
                iac = false;
                command += ch;
                processTelnetCommand( command );
                //this could have set receivedGA to true; we'll handle that later
                command = "";
            }
        }
        else
        {
            if( ch != '\r' ) cleandata += ch;
        }

        if( recvdGA )
        {
            mGA_Driver = true;
            if( mCommands > 0 )
            {
                mCommands--;
                if( networkLatencyTime.elapsed() > 2000 )
                {
                    mCommands = 0;
                }
            }

            if( mUSE_IRE_DRIVER_BUGFIX || mLF_ON_GA )
            {
                cleandata.push_back('\n');//part of the broken IRE-driver bugfix to make up for broken \n-prepending in unsolicited lines, part #2 see line 628
            }
            recvdGA = false;
            gotPrompt( cleandata );
            cleandata = "";
        }
    }//for

    if( cleandata.size() > 0 )
    {
       gotRest( cleandata );
    }

    mpHost->mpConsole->finalize();
    if( loadingReplay ) _loadReplay();
}

void cTelnet::handle_socket_signal_readyRead()
{
    mpHost->mInsertedMissingLF = false;

    if( mWaitingForResponse )
    {
        double time = networkLatencyTime.elapsed();
        networkLatency = time/1000;
        mWaitingForResponse = false;
    }

    char buffer[100010];

    int amount = socket.read( buffer, 100000 );
    buffer[amount+1] = '\0';
    if( amount == -1 ) return;
    if( amount == 0 ) return;

    int datalen = amount;
    char * pBuffer = buffer;
    if( mNeedDecompression )
    {
        datalen = decompressBuffer( pBuffer, amount );
    }
    buffer[datalen] = '\0';
    #ifdef DEBUG
        cout<<"got<"<<pBuffer<<">"<<endl;
    #endif
    if( mpHost->mpConsole->mRecordReplay )
    {
        mpHost->mpConsole->mReplayStream << timeOffset.elapsed()-lastTimeOffset;
        mpHost->mpConsole->mReplayStream << datalen;
        mpHost->mpConsole->mReplayStream.writeRawData( &buffer[0], datalen );
    }

    string cleandata = "";
    recvdGA = false;
    for( int i = 0; i < datalen; i++ )
    {
        char ch = buffer[i];

        if( iac || iac2 || insb || (ch == TN_IAC) )
        {
            #ifdef DEBUG
                cout <<" SERVER SENDS telnet command "<<(unsigned int)_ch<<endl;
            #endif
            if( ! (iac || iac2 || insb) && ( ch == TN_IAC ) )
            {
                iac = true;
                command += ch;
            }
            else if( iac && (ch == TN_IAC) && (!insb) )
            {
                //2. seq. of two IACs
                iac = false;
                cleandata += ch;
                command = "";
            }
            else if( iac && (!insb) && ((ch == TN_WILL) || (ch == TN_WONT) || (ch == TN_DO) || (ch == TN_DONT)))
            {
                //3. IAC DO/DONT/WILL/WONT
                iac = false;
                iac2 = true;
                command += ch;
            }
            else if(iac2)
            {
                //4. IAC DO/DONT/WILL/WONT <command code>
                iac2 = false;
                command += ch;
                processTelnetCommand( command );
                command = "";
            }
            else if( iac && (!insb) && (ch == TN_SB) )
            {
                //5. IAC SB
                iac = false;
                insb = true;
                command += ch;
            }
            else if( iac && (!insb) && (ch == TN_SE) )
            {
                //6. IAC SE without IAC SB - error - ignored
                command = "";
                iac = false;
            }
            else if( insb )
            {
                /*if( buffer[i] == static_cast<char>(200) )
                {
                    cout << "got atcp? ";
                    if( i > 1 )
                    {
                        if( ( buffer[i-2] == TN_IAC ) && ( buffer[i-1] == TN_SB ) )
                        {
                            atcp_msg = true;
                            cout << " yes"<<endl;
                        }
                        else
                            cout << "no"<<endl;
                    }
                }
                else*/
                if( ! mNeedDecompression )
                {
                    // IAC SB COMPRESS WILL SE for MCCP v1 (unterminated invalid telnet sequence)
                    // IAC SB COMPRESS2 IAC SE for MCCP v2
                    if( mMCCP_version_1 || mMCCP_version_2 )
                    {
                        char _ch = buffer[i];
                        if( (_ch == OPT_COMPRESS ) || (_ch == OPT_COMPRESS2 ) )
                        {
                            bool _compress = false;
                            if( ( i > 1 ) && ( i+2 < datalen ) )
                            {
                                cout << "checking mccp start seq..." << endl;
                                if( ( buffer[i-2] == TN_IAC ) && ( buffer[i-1] == TN_SB ) && ( buffer[i+1] == TN_WILL ) && ( buffer[i+2] == TN_SE ) )
                                {
                                    cout << "MCCP version 2 starting sequence" << endl;
                                    _compress = true;
                                }
                                if( ( buffer[i-2] == TN_IAC ) && ( buffer[i-1] == TN_SB ) && ( buffer[i+1] == TN_IAC ) && ( buffer[i+2] == TN_SE ) )
                                {
                                    cout << "MCCP version 1 starting sequence" << endl;
                                    _compress = true;
                                }
                                cout << (int)buffer[i-2]<<","<<(int)buffer[i-1]<<","<<(int)buffer[i]<<","<<(int)buffer[i+1]<<","<<(int)buffer[i+2]<<endl;
                            }
                            if( _compress )
                            {
                                cout << "COMPRESSION START sequence found"<<endl;
                                mNeedDecompression = true;
                                // from this position in stream onwards, data will be compressed by zlib
                                cout << "starting ZLIB decompression. ";
                                gotRest( cleandata );
                                cleandata = "";
                                initStreamDecompressor();
                                pBuffer += 3;
                                //mWaitingForCompressedStreamToStart = false;
                                int restLength = datalen - i - 3;
                                if( restLength > 0 )
                                {
                                    datalen = decompressBuffer( pBuffer, restLength );
                                }
                                i = 0;
                                goto MAIN_LOOP_END;
                            }
                        }
                    }
                }
                //7. inside IAC SB

                command += ch;
                if( iac && (ch == TN_SE) )  //IAC SE - end of subcommand
                {
                    processTelnetCommand( command );
                    command = "";
                    iac = false;
                    insb = false;
                }
                if(iac) iac = false;
                else if( ch == TN_IAC ) iac = true;
            }
            else
            //8. IAC fol. by something else than IAC, SB, SE, DO, DONT, WILL, WONT
            {
                iac = false;
                command += ch;
                processTelnetCommand( command );
                //this could have set receivedGA to true; we'll handle that later
                command = "";
            }
        }
        else
        {
            if( ch != '\r' ) cleandata += ch;
        }
MAIN_LOOP_END: ;
        if( recvdGA )
        {
            if( ! mFORCE_GA_OFF ) //FIXME: wird noch nicht richtig initialisiert
            {
                mGA_Driver = true;
                if( mCommands > 0 )
                {
                    mCommands--;
                    if( networkLatencyTime.elapsed() > 2000 )
                    {
                        mCommands = 0;
                    }
                }
                cleandata.push_back('\xff');
                recvdGA = false;
                gotPrompt( cleandata );
                cleandata = "";
            }
            else
            {
                if( mLF_ON_GA ) //TODO: reenable option in preferences
                {
                    cleandata.push_back('\n');
                }
            }
        }
    }//for

    if( cleandata.size() > 0 )
    {
       gotRest( cleandata );
    }
    mpHost->mpConsole->finalize();
    lastTimeOffset = timeOffset.elapsed();
}



