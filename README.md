# State Bus (Flux): The Real-Time Lifecycle Engine

The State Bus is a high-performance, binary-protocol "Dumb Engine" designed for ultra-low latency state synchronization. Built in C, it serves as the foundational nervous system for collaborative ecosystems that require instantaneous updates and ephemeral data management.

## The Philosophy: "Dumb" for Speed
Unlike traditional message brokers (Kafka, RabbitMQ) that focus on persistence and heavy processing, the **State Bus** focuses on **Routing and Decay**.
- **Mechanism over Policy:** The server is application-agnostic. It doesn't interpret your data; it move bytes based on ```scope_id``` and ```packet_type```.
- **Hardware-Level TTL:** Native support for Time-To-Live (TTL) functionality, ensuring data expires at the protocol level rather than during background database cleanup.
- **Zero-Copy Mentality:** Designed to handle high-concurrency bursts (500+ packets) with minimal CPU jitter.

## Features
- **Binary Protocol:** Packed C structs for minimal bandwidth overhead.
- **Strict Byte-Ordering:** Big-Endian enforcement (`htonl`, `htobe64`) for cross-platform compatibility (Mobile/Web/Desktop).
- **Non-Blocking I/O:** Powered by a high-efficiency epoll loop.
- **Identity Gate:** Server-side authority for `sender_id` to prevent identity spoofing.
- **Stress Tested:** Successfully handles 500+ packet bursts without sync loss.
- **Scope-Based Routing:** Logical isolation of data (Rooms/Channels) via ```scope_id```.

## Auth Service Integration
**Auth Request** - engine sends this when a client presents a token. Always exactly 256 bytes, zero padded.
```
Bytes 0-1:      token_len (uint16_t, network byte order)
Bytes 2-255:    token data (zero-padded to fill)
```
**Auth Response** - auth service sends this back. Always exactly 8 bytes.
```
Byte 0:     valid (uint8_t, 1=authenticated 0=rejected)
Byte 1-4:   user_id (uint32_t, network byte order)
Byte 5-7:   reserved (must be zero)
```
Auth services must respond within 500ms or the connection is rejected. This protects the engine from a hung auth service stalling new connections.

```ENGINE_AUTH_SOCKET``` environment variable controls the path. Empty string means use the development hook (accepts everything).

## Quick Start
**1. Build the Ecosystem**
The project uses a ```Makefile``` to handle complex linking and flags:
```
make clean && make
```

**2. Launch the Bus (Server)**
Start the central router. By default, it listens on port ```8080```.
```
./server
```

**3. Connect a Node (Client):**
Open multiple terminals to simulate real-time collaboration:
```
./client
```
