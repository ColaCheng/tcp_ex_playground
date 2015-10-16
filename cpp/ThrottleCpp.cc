 #include <sys/types.h>
 #include <sys/socket.h>
 #include <sys/uio.h>
 #include <sys/select.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <string.h>
 #include <unistd.h>
 #include <stdio.h>

 #include <iostream>
 #include <functional>
 #include <cstdint>
 #include <chrono>
 #include <thread>

namespace
{
  //
  // to help freeing C resources
  //
  struct on_destruct
  {
    std::function<void()> fun_;
    on_destruct(std::function<void()> fun) : fun_(fun) {}
    ~on_destruct() { fun_(); }
  };

  //
  // for measuring ellapsed time and print statistics
  //
  struct timer
  {
    typedef std::chrono::high_resolution_clock      highres_clock;
    typedef std::chrono::time_point<highres_clock>  timepoint;
    
    timepoint  start_;
    uint64_t   iteration_;

    timer(uint64_t iter) : start_{highres_clock::now()}, iteration_{iter} {}

    int64_t spent_usec()
    {
      using namespace std::chrono;
      timepoint now{highres_clock::now()};
      return duration_cast<microseconds>(now-start_).count();
    }

    ~timer()
    {
      using namespace std::chrono;
      timepoint now{highres_clock::now()};
      
      uint64_t  usec_diff     = duration_cast<microseconds>(now-start_).count();
      double    call_per_ms   = iteration_*1000.0     / ((double)usec_diff);
      double    call_per_sec  = iteration_*1000000.0  / ((double)usec_diff);
      double    us_per_call   = (double)usec_diff     / (double)iteration_;

      std::cout << "elapsed usec=" << usec_diff
                << " avg(usec/call)=" << us_per_call
                << " avg(call/msec)=" << call_per_ms
                << " avg(call/sec)=" << call_per_sec
                << std::endl;
    }
  };
}


int main(int argc, char ** argv)
{
  try
  {
    // create a TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if( sockfd < 0 )
    {
      throw "can't create socket";
    }
    on_destruct close_sockfd( [sockfd](){ close(sockfd); } );

    // server address (127.0.0.1:8002)
    struct sockaddr_in server_addr;
    ::memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(8002);  

    // connect to server
    if( connect(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1 )
    {
      throw "failed to connect to server at 127.0.0.1:8002";
    }

    // prepare data
    char      data[]        = "Hello";
    uint64_t  id            = 0;
    uint32_t  len           = 5;
    int64_t   last_ack      = -1;

    struct iovec data_iov[3] = {
      { (char *)&id,   8 }, // id
      { (char *)&len,  4 }, // len
      { data,          5 }  // data
    };


    //
    // this lambda function checks if we have received a new ACK.
    // if we did then it checks the content and returns the max
    // acknowledged ID. this supports receiving multiple ACKs in
    // a single transfer.
    //
    auto check_ack = [sockfd](int64_t last_ack) {
      int64_t ret_ack = last_ack;
      fd_set fdset;
      FD_ZERO(&fdset);
      FD_SET(sockfd, &fdset);
      
      // give 1 ms to the acks to arrive
      struct timeval tv { 0, 1000 };
      int select_ret = select( sockfd+1, &fdset, NULL, NULL, &tv );
      if( select_ret < 0)
      {
        throw "failed to select, socket error?";
      }
      if( select_ret > 0 && FD_ISSET(sockfd,&fdset) )
      {
        // max 2048 acks that we handle in one check
        size_t alloc_bytes = 12 * 2048;
        std::unique_ptr<uint8_t[]> ack_data{new uint8_t[alloc_bytes]};

        //
        // let's receive what has arrived. if there are more than 2048
        // ACKs waiting, then the next loop will take care of them
        //

        auto recv_ret = recv(sockfd, ack_data.get(), alloc_bytes, 0);
        if( recv_ret < 0 )
        {
          throw "failed to recv, socket error?";
        }
        if( recv_ret > 0 )
        {
          for( size_t pos=0; pos<recv_ret; pos+=12 )
          {
            uint64_t id = 0;
            uint32_t skipped = 0;
            // copy the data to the variables above
            memcpy(&id, ack_data.get()+pos, sizeof(id) );
            memcpy(&skipped, ack_data.get()+pos+sizeof(id), sizeof(skipped) );
            
            // check the ACKs
            if( (ret_ack + skipped + 1) != id )
            {
              throw "missing ack";
            }
            ret_ack = id;
          }
        }
      }
      return ret_ack;
    };
    
    for( int i=0; i<20; ++i )
    {
      size_t iter = 100000;
      timer t(iter);
      int64_t checked_at_usec = 0;
      
      // send data in a loop
      for( size_t kk=0; kk<iter; ++kk )
      {
        if( writev(sockfd, data_iov, 3) != 17 )
        {
          throw "failed to send data";
        }
        
        //
        // check time after every 1000 send so I reduce
        // OS calls by not querying time too often
        //
        if( (kk%1000) == 0 )
        {
          //
          // check if at least 30 msecs has ellapsed since the
          // last ACK check
          //
          int64_t spent_usec = t.spent_usec();
          if( spent_usec > (checked_at_usec+30000) )
          {
            last_ack = check_ack(last_ack);
            checked_at_usec = spent_usec;
          }          
        }
        ++id;
      }
      
      // wait for all outstanding ACKs
      while( last_ack < (id-1) )
        last_ack = check_ack(last_ack);
    }
    
    while( last_ack < (id-1) )
    {
      last_ack = check_ack(last_ack);
      if( last_ack != id )
      {
        std::cerr << "last_ack=" << last_ack << " id=" << id << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }
  }
  catch( const char * msg )
  {
    perror(msg);
  }
  return 0;
}
