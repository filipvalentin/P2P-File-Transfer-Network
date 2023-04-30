# P2P-File-Transfer-Network

A faculty project, involving network programming where each node is provided with its own server and client. <br>
The client provides functionalities regarding connections to servers, making requests and single/multi downloads, while the server can expose files to the entire network, each server routing & processing requests to other peers in an optimal manner using caches and backtrace stacks in order to fulfill search requests.

How it works: Every connection between server & client and server & server first checks if a/the server is available (isn't scheduled for closing (so that busy threads won't be stoped unexpectedly)), then it sends a request code and based on it both interlocutors communicate based on a predefined protocol.
Searching in the network: a client connects, the request is queued for broadcasting while another queue is used to store the responses, including the current server's. 

Before being queued though, the request gets a stack of its own in which every server will write its own IP; this functionality provides a backtrace stack that the same servers will use (in the broadcasting threads) in order to try to route the responses back to the origin. 

In the same time request and response caches are set up, these caches will prevent routing a request back to the sender.

In parralel the broadcasting/cascading threads will consume non-cached requests in a synchronous manner, cache them and send them to every peer-server connected. This might arise a question: why check the cache here and not when receiving it? Because when receiving the same new request twice from different servers at the same time, it woudln't be present in the cache so that it would be excluded, so that's why this thread manages what requests to cascade or not. On this topic, every request has a Time To Live, and peers will keep account for it.

Then the peer-server will receive the search request, it will add its own IP to the backtrace stack, queue the request and process it. Then the broadcast threads will continue cascading. At the same time every server uses a copy of the backtrace stack in order to route their own responses back, while the server the client connected to will gather the responses and send them back to the client.

If anything fails, the servers are programmed to cascade to every server they may find, but not actively trying to route the responses in an optimal manner. In these situation, there could be made an improvement such that any server will try to reconnect to the server the client is connected to, send the data and cache it such that other requests will be ignored. Although smart, it would raise complexity.

After the client receives all the responses it could get, it'll connect to the desired server via the terminal commands, keeping in mind that although servers communicate across private IPs (and those are the only IPs they are aware of), in order for clients/peers to connect to a remote server outside localhost the public IP is needed.
