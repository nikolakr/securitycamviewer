#include "MjpegClient.h"

#include <QTimer>

MjpegClient::MjpegClient(QObject *parent) 
	: QThread(parent)
	, m_socket(0)
	, m_boundary("")
	, m_firstBlock(true)
	, m_dataBlock("")
	, m_autoReconnect(true)
	
{
#ifdef MJPEG_TEST
	m_label = new QLabel();
	m_label->setWindowTitle("MJPEG Test");
	m_label->setGeometry(QRect(0,0,320,240));
	m_label->show();
#endif
}
MjpegClient::~MjpegClient()
{
}
  
bool MjpegClient::connectTo(const QString& host, int port, QString url)
{
	if(url.isEmpty())
		url = "/";
		
	m_host = host;
	m_port = port;
	m_url = url;
	
	if(m_socket)
	{
		m_socket->abort();
		delete m_socket;
		m_socket = 0;
	}
		
	m_socket = new QTcpSocket(this);
	connect(m_socket, SIGNAL(readyRead()),    this,   SLOT(dataReady()));
	connect(m_socket, SIGNAL(disconnected()), this,   SLOT(lostConnection()));
	connect(m_socket, SIGNAL(disconnected()), this, SIGNAL(socketDisconnected()));
	connect(m_socket, SIGNAL(connected()),    this, SIGNAL(socketConnected()));
	connect(m_socket, SIGNAL(connected()),    this,   SLOT(connectionReady()));
	connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SIGNAL(socketError(QAbstractSocket::SocketError)));
	connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(lostConnection()));
	
	m_socket->connectToHost(host,port);
	
	return true;
}

void MjpegClient::connectionReady()
{
	char data[256];
	sprintf(data, "GET %s HTTP/1.0\r\n\r\n",qPrintable(m_url));
	m_socket->write((const char*)&data,strlen((const char*)data));
}

void MjpegClient::log(const QString& str)
{
	qDebug() << "MjpegClient::log(): "<<str;
}

void MjpegClient::lostConnection()
{
	if(m_autoReconnect)
		QTimer::singleShot(1000,this,SLOT(reconnect()));
}

void MjpegClient::reconnect()
{
	log(QString("Attempting to reconnect to http://%1:%2%3").arg(m_host).arg(m_port).arg(m_url));
	connectTo(m_host,m_port,m_url);
}

void MjpegClient::dataReady()
{
	while(m_socket && m_socket->bytesAvailable())
	{
		QByteArray bytes = m_socket->readAll();
		if(bytes.size() > 0)
		{
			m_dataBlock.append(bytes);
			processBlock();
		}
	}
	
// 	if(m_socket && m_socket->bytesAvailable())
// 	{
// 		QTimer::singleShot(0, this, SLOT(dataReady()));
// 	}
}

void MjpegClient::processBlock()
{
	if(m_boundary.isEmpty())
	{
		// still waiting for boundary string defenition, check for content type in data block
		char * ctypeString = 0;
		if(m_dataBlock.contains("Content-Type:"))
		{
			ctypeString = "Content-Type:";
		}
		else
		if(m_dataBlock.contains("content-type:"))
		{
			// allow for buggy servers (some IP cameras - trendnet, I'm looking at you!) 
			// sometimes dont use proper case in their headers.
			ctypeString = "content-type:";
		}
		
		if(ctypeString)
		{
			int ctypeIdx = m_dataBlock.indexOf(ctypeString);
			if(ctypeIdx < 0)
			{
				qDebug() << "Error: Can't find content-type index in data block, exiting.";
				exit();
				return;
			}
			
			static QString boundaryMarker = "boundary=";
			int boundaryStartIdx = m_dataBlock.indexOf(boundaryMarker,ctypeIdx);
			if(boundaryStartIdx < 0)
			{
				qDebug() << "Error: Can't find boundary index after the first content-type index in data block, exiting.";
				exit();
				return;
			}
			
			int eolIdx = m_dataBlock.indexOf("\n",boundaryStartIdx);
			int pos = boundaryStartIdx + boundaryMarker.length();
			m_boundary = m_dataBlock.mid(pos, eolIdx - pos);
			m_boundary.replace("\r","");
// 			qDebug() << "processBlock(): m_boundary:"<<m_boundary<<", pos:"<<pos<<", eolIdx:"<<eolIdx;
		}
	}
	else
	{
		// we have the boundary string defenition, check for the boundary string in the data block.
		// If found, grab the data from the start of the block up to and including the boundary, leaving any data after the boundary in the block.
		// What we then have to process could look:
		// Content-Type.....\r\n(data)--(boundary)
		
		// If its the first block, we wait for the boundary, but discard it since the first block is the one that contains the server headers
		// like the boundary definition, Server:, Connection:, etc.
		int idx = m_dataBlock.indexOf(m_boundary);
		
 		while(idx > 0)
 		{
			QByteArray block = m_dataBlock.left(idx);
			
			int blockAndBoundaryLength = idx + m_boundary.length();
			m_dataBlock.remove(0,blockAndBoundaryLength);
			//qDebug() << "processBlock(): block length:"<<block.length()<<", blockAndBoundaryLength:"<<blockAndBoundaryLength;
			
			if(m_firstBlock)
			{
				//QString string = block;
				//qDebug() << "processBlock(): Discarding block since first block flag is true. Dump of block:\n"<<string;
				m_firstBlock = false;
			}
			else
			{
				static const char * eol1 = "\n\n";
				static const char * eol2 = "\r\n\r\n";
				int headerLength = 0;
				if(block.contains(eol2))
					headerLength = block.indexOf(eol2,0) + 4;
				else
				if(block.contains(eol1))
					headerLength = block.indexOf(eol1,0) + 2;
				
				if(headerLength)
				{
					//QString header = block.left(headerLength);
					
					block.remove(0,headerLength);
					
					// Block should now be just data
					//qDebug() << "processBlock(): block length:"<<block.length()<<", headerLength:"<<headerLength<<", header:"<<header;
					
					if(block.length() > 0)
					{
					
						QImage frame = QImage::fromData(block);
						if(!m_autoResize.isNull() && m_autoResize != frame.size())
							frame = frame.scaled(m_autoResize);
						emit newImage(frame);
						
						#ifdef MJPEG_TEST
						QPixmap pix = QPixmap::fromImage(frame);
						m_label->setPixmap(pix);
						m_label->resize(pix.width(),pix.height());
						#endif
					}
				}

			}
			
// 			// check for another boundary string in the data before exiting from processBlock()
 			idx = m_dataBlock.indexOf(m_boundary);
 		}
		
	}
	
	//qDebug() << "processBlock(): End of processing, m_dataBlock.size() remaining:"<<m_dataBlock.size();
}

void MjpegClient::exit()
{
	if(m_socket)
	{
		m_socket->abort();
		m_socket->disconnectFromHost();
		//m_socket->waitForDisconnected();
		m_socket->deleteLater();
		//delete m_socket;
		m_socket = 0;
	}
}


