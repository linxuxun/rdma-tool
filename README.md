# RDMA Tool

RDMA CM example in pure C11.

librdmacm + libibverbs:
- Server/client mode
- RDMA Write (one-sided)
- RDMA Read  (one-sided)
- Send/Recv messaging
- epoll event loop
- Signal handling

## Build

    sudo apt install librdmacm-dev libibverbs-dev
    make

## Usage

    ./build/rdma_tool -s -i 0.0.0.0 -p 12345
    ./build/rdma_tool -c -i 127.0.0.1 -p 12345 -l debug

## Client Commands

    info           # show peer MR info
    write <data>   # RDMA Write to peer
    read           # RDMA Read from peer
    text <msg>     # send text
    quit           # disconnect
