
#include <thread>
#include <mutex>

#include <iostream>
#include <vector>
#include <map>

#include <sys/types.h>
#include <windows.h>
//#include <sys/socket.h>
//#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <climits>

#define PORT_NUM 8932

typedef unsigned long long int uint64; // целочисленный неотрицательный 64-х битный счетчик
typedef unsigned char uchar;

struct GeneratorSequence
{
	struct GeneratorParams
	{
		uint64 nStartVal = 0;
		uint64 nStep = 0;
		// uint64 nValue = 0; // похоже по тз - типо нельзя здесь хранить... вродее как напрашивается...
		void print() const
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

	typedef std::map<std::string, GeneratorParams> GPMap;

	GPMap genParams = {
						{"seq1", GeneratorParams()},
						{"seq2", GeneratorParams()},
						{"seq3", GeneratorParams()}
					  };
	void print()
	{
		for ( const auto& gp : genParams )
			gp.second.print();
	}

	void parseParams( const std::string command )
	{
		try
		{
			std::string seq = command.substr(0,4);
			std::string params = command[4] == ' ' ? command.substr(5, command.size()) : "";

			genParams.at(seq).nStep     = std::stoi( params.substr(params.find(' '), params.size()) );
			genParams.at(seq).nStartVal = std::stoi( params.substr(0, params.find(' ')) );
		}
		catch (std::exception e)
		{
			printf( "%s Error: invalid command\n", e.what() );
		};
	}

};


typedef std::map<int, GeneratorSequence> GeneratorArray;

void processSocked(const int sockedID, GeneratorArray& arr, std::mutex& m_arr, bool& bShutdownServer)
{
	char buf[1024];
	int bytes_read = -1;
	std::map<std::string, uint64> process_values;

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
			GeneratorSequence generator = arr[ sockedID ];
			m_arr.unlock();

			// init start values
			for ( const auto& gp : generator.genParams )
				process_values[ gp.first ] = gp.second.nStartVal;

			int nIter = 0;

			while ( nIter < 10 ) // по ТЗ не определено количество вычислений, ставить бесконечный цикл усыпая клиента беспрерывным выводом как-то глупо, ставим понравившееся число итераций
			{
				std::string collect_values;
				for (const auto& qv : process_values)
					collect_values.append( std::to_string (qv.second) + " " );
				collect_values.append("\n");

				send(sockedID, collect_values.c_str(), collect_values.size(), 0);

				for ( auto& qv : process_values )
					qv.second = generator.genParams[qv.first].calc( qv.second );

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

	WSAData data;
	int wsa_return_code = WSAStartup (MAKEWORD(1, 1), &data);
	if(wsa_return_code != 0)
	{
		printf ("Error WSAStartup code: %d", WSAGetLastError () );
		exit(1);
	}
	// без WSAStartup() функция socket(AF_INET, SOCK_STREAM, 0)
	// возвращает "-1", а WSAGetLastError() возвращает WSANOTINITIALISED
	// при этом perror("socket") выводит "socket: No error"
	// http://www.frolov-lib.ru/books/bsp/v23/ch5_2.html
	// http://www.frolov-lib.ru/books/bsp/v23/ch5_3.html

	MasterSocked = socket(AF_INET, SOCK_STREAM, 0);
	if( MasterSocked < 0 )
	{
		printf ("WSA Error code: %d", WSAGetLastError () );
//		perror("socket");
		exit(1);
	}

	// быстрый обход проблемы "Address already in use" - при завершении сервера по Ctrl+C, чтобы не ждать тайм-аут освобождения порта от ОС
	// You can use setsockopt() to set the SO_REUSEADDR socket option, which explicitly allows a process to bind to a port which remains in TIME_WAIT (it still only allows a single process to be bound to that port).
	// This is the both the simplest and the most effective option for reducing the "address already in use" error.

//	int opt = 1;
//	if (setsockopt (MasterSocked, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt)) == -1)
//		perror("setsockopt");

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
