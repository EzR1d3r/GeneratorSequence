
#include <thread>
#include <mutex>

#include <iostream>
#include <vector>
#include <map>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <climits>

#define PORT_NUM 8932

typedef unsigned long long int uint64; // целочисленный неотрицательный 64-х битный счетчик

struct GeneratorSequence
{
	struct GeneratorParams
	{
		uint64 nStartVal = 0;
		uint64 nStep = 0;
		// uint64 nValue = 0; // похоже по тз - типо нельзя здесь хранить... вродее как напрашивается...
		void print()
		{
			printf( "start = %llu step = %llu \n", nStartVal, nStep );
		}
		uint64 calc( uint64 currVal )
		{
//			Если в командах 1, 2, 3 любой из параметров (начальное значение и/или шаг)
//			будет указан как = 0, то программа не должна генерировать данную
//			последовательность; - Непонятно как не должна, цикл надо остановить или выдать 0 - делаем пока 0
			uint64 kZero = nStartVal * nStep;
			currVal = kZero != 0 ? currVal + nStep : 0;
			if ( currVal > ULLONG_MAX )
				currVal = nStartVal;
			return currVal;
		}
	};

	std::vector<GeneratorParams> genParams = { GeneratorParams(), GeneratorParams(), GeneratorParams() };
	void print()
	{
		for ( auto gp : genParams )
			gp.print();
	}
	void parseParams( const std::string sVal )
	{
		// а вот если бы можно было использовать QString код был бы в 3 раза короче...
		int nSeqIndex;

		std::string sSeqIndex = sVal.substr( sVal.find( "seq" ) + 3, 1 ); // знаем точно, что среди команд только seq1, seq2, seq3
		try
		{
			nSeqIndex = std::stoi( sSeqIndex );
		}
		catch (std::invalid_argument)
		{
			nSeqIndex = -1;
		}

		if ( nSeqIndex < 1 || nSeqIndex > 3 ) return; // ignore all other - only 1,2,3

		int nFSpace = sVal.find( ' ' );
		int nLSpace = sVal.rfind( ' ' );

		std::string sStartVal = sVal.substr( nFSpace + 1, nLSpace-1-nFSpace);
		std::string sStep = sVal.substr( nLSpace + 1, sVal.size());

		try
		{
			genParams[ nSeqIndex-1 ].nStartVal = std::stoi( sStartVal );
			genParams[ nSeqIndex-1 ].nStep = std::stoi( sStep );
		}
		catch (std::invalid_argument)
		{
			genParams[ nSeqIndex-1 ].nStartVal = 0;
			genParams[ nSeqIndex-1 ].nStep = 0;
		}

		//std::cout << sVal << nSeqIndex << nFSpace << nLSpace << " " << sStartVal << " " << sStep << std::endl; // only for debug string manipulation
	}
};


typedef std::map<int, GeneratorSequence> GeneratorArray;

void processSocked(const int sockedID, GeneratorArray& arr, std::mutex& m_arr, bool& bShutdownServer)
{
	char buf[1024];
	int bytes_read = -1;
	std::vector<uint64> seqValues = { 0, 0, 0 };

	// Work with shared data - clients settings container
	m_arr.lock();
	arr.insert( std::pair<int, GeneratorSequence>( sockedID, GeneratorSequence() ) );
	m_arr.unlock();

	// only for debug
	for ( auto& map_pair: arr )
		map_pair.second.print();

	std::cout << "Client connected, Socked ID = " << sockedID << std::endl;

	do
	{
		bytes_read = recv(sockedID, buf, 1024, 0);

		std::string str( buf, bytes_read );

		// process commands
		if ( str.find("exit") != std::string::npos )
			break;
		else if ( str.find("close_server") != std::string::npos ) // experemental, not works good, need more handler
		{
			bShutdownServer = true;
			break;
		}
		else if ( str.find("export seq") != std::string::npos )
		{
			m_arr.lock();
			GeneratorSequence seq = arr[ sockedID ];
			m_arr.unlock();

			// init start values
			for ( int i = 0; i < 3; ++i )
				seqValues[i] = seq.genParams[i].nStartVal;

			int nIter = 0;
			char buf[128];
			int bytes_count = 0;

			while ( nIter < 10 ) // по ТЗ не определено количество вычислений, ставить бесконечный цикл усыпая клиента беспрерывным выводом как-то глупо, ставим понравившееся число итераций
			{
				bytes_count = sprintf( buf, "%llu %llu %llu\n", seqValues[0], seqValues[1], seqValues[2] );
				send(sockedID, buf, bytes_count, 0);

				for ( int i = 0; i < 3; ++i )
					seqValues[i] = seq.genParams[i].calc( seqValues[i] );

				++nIter;
			}
		}
		else if ( str.find("seq") != std::string::npos )
		{
			m_arr.lock();
			GeneratorSequence seq = arr[ sockedID ];
			seq.parseParams( str );
			seq.print();
			arr[ sockedID ] = seq; // not optimal by value, but not critical now...
			m_arr.unlock();
		}

	} while ( bytes_read != 0);

	std::cout << "Client disconnected, Socked ID = " << sockedID << std::endl;
	close( sockedID );

	m_arr.lock();
	arr.erase( arr.find( sockedID ) );
	m_arr.unlock();

	// only for debug
	for ( auto& map_pair: arr )
		map_pair.second.print();
}

int main()
{
	GeneratorArray g_Generators;
	std::mutex m_Generators;
	std::vector<std::thread*> g_Threads;

	bool bAppExit = false;
	int ClientSocked, MasterSocked;
	struct sockaddr_in addr;

	MasterSocked = socket(AF_INET, SOCK_STREAM, 0);
	if( MasterSocked < 0 )
	{
		perror("socket");
		exit(1);
	}

	// быстрый обход проблемы "Address already in use" - при завершении сервера по Ctrl+C, чтобы не ждать тайм-аут освобождения порта от ОС
	// You can use setsockopt() to set the SO_REUSEADDR socket option, which explicitly allows a process to bind to a port which remains in TIME_WAIT (it still only allows a single process to be bound to that port).
	// This is the both the simplest and the most effective option for reducing the "address already in use" error.

	int opt = 1;
	if (setsockopt (MasterSocked, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt)) == -1)
		perror("setsockopt");

	addr.sin_family = AF_INET;
	addr.sin_port = htons( PORT_NUM );
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if(bind(MasterSocked, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		perror("bind");
		exit(2);
	}

	listen(MasterSocked, 1);

	// сейчас выход из программы возможен по Ctrl+C или командой close_server от любого клиента,
	// но во втором случае выход произойдет с задержкой в одно подключение, т.к. главный поток уже ожидает в блокирующей ф-ции accept
	// для корректного выхода можно сделать иначе, но такого задания не было по тз...
	while( !bAppExit )
	{
		ClientSocked = accept(MasterSocked, NULL, NULL);
		if(ClientSocked < 0)
		{
			perror("accept");
			exit(3);
		}

		std::thread * pThread = new std::thread(processSocked, ClientSocked, std::ref( g_Generators ), std::ref( m_Generators ), std::ref( bAppExit ) );

		g_Threads.emplace_back( pThread );
		std::cout << bAppExit << std::endl;
	}

	for ( std::thread * pThread: g_Threads )
		pThread->join();

	close(MasterSocked);

	g_Threads.clear();

	return 0;
}
