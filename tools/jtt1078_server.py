#!/usr/bin/env python3
"""
JT/T 1078 Protocol Test Server
简单的 JT/T 1078 服务器用于测试和调试

Usage:
    python3 jtt1078_server.py [port]
    
Default port: 6605
"""

import socket
import struct
import sys
import threading
import time
from datetime import datetime

# JT/T 1078 Constants
JTT1078_HEADER_FLAG = 0x30316364  # "01cd"

DATA_TYPES = {
    0x00: "Video I-frame",
    0x01: "Video P-frame", 
    0x02: "Video B-frame",
    0x03: "Audio frame",
    0x04: "Transparent data"
}

SUBPACKAGE_TYPES = {
    0b00: "Atomic",
    0b01: "First",
    0b10: "Last",
    0b11: "Middle"
}

class JTT1078Packet:
    """JT/T 1078 packet parser"""
    
    def __init__(self, data):
        self.raw_data = data
        self.parse()
    
    def parse(self):
        """Parse JT/T 1078 header"""
        if len(self.raw_data) < 30:
            raise ValueError(f"Packet too short: {len(self.raw_data)} bytes")
        
        # Parse header (30 bytes)
        self.header_flag = struct.unpack('>I', self.raw_data[0:4])[0]
        if self.header_flag != JTT1078_HEADER_FLAG:
            raise ValueError(f"Invalid header flag: 0x{self.header_flag:08X}")
        
        # RTP header
        rtp_byte = self.raw_data[4]
        self.rtp_version = (rtp_byte >> 6) & 0x03
        self.rtp_padding = (rtp_byte >> 5) & 0x01
        self.rtp_extension = (rtp_byte >> 4) & 0x01
        self.rtp_csrc_count = rtp_byte & 0x0F
        
        pt_byte = self.raw_data[5]
        self.payload_type = pt_byte & 0x7F
        self.marker = (pt_byte >> 7) & 0x01
        
        self.packet_seq = struct.unpack('>H', self.raw_data[6:8])[0]
        
        # Extended header
        self.sim_bcd = self.raw_data[12:18]
        self.sim = self.bcd_to_sim(self.sim_bcd)
        self.channel = self.raw_data[18]
        
        data_type_byte = self.raw_data[19]
        self.data_type = data_type_byte & 0x0F
        self.subpackage = (data_type_byte >> 4) & 0x03
        
        self.timestamp = struct.unpack('>Q', self.raw_data[20:28])[0]
        
        # Frame intervals
        self.last_i_interval = struct.unpack('>H', self.raw_data[28:30])[0]
        self.last_interval = struct.unpack('>H', self.raw_data[30:32])[0]
        
        # Data length
        self.data_length = struct.unpack('>H', self.raw_data[32:34])[0]
        
        # Payload
        self.payload = self.raw_data[34:]
        
        if len(self.payload) != self.data_length:
            print(f"⚠️  Warning: Payload size mismatch. Expected {self.data_length}, got {len(self.payload)}")
    
    @staticmethod
    def bcd_to_sim(bcd_bytes):
        """Convert 6 bytes BCD to 12 digit SIM string"""
        sim = ""
        for byte in bcd_bytes:
            high = (byte >> 4) & 0x0F
            low = byte & 0x0F
            sim += str(high) + str(low)
        return sim
    
    def __str__(self):
        """Human readable packet info"""
        lines = []
        lines.append("=" * 60)
        lines.append(f"📦 JT/T 1078 Packet #{self.packet_seq}")
        lines.append("=" * 60)
        lines.append(f"  SIM:         {self.sim}")
        lines.append(f"  Channel:     {self.channel}")
        lines.append(f"  Data Type:   {DATA_TYPES.get(self.data_type, f'Unknown({self.data_type})')}")
        lines.append(f"  Subpackage:  {SUBPACKAGE_TYPES.get(self.subpackage, f'Unknown({self.subpackage})')}")
        lines.append(f"  Timestamp:   {self.timestamp} ms")
        lines.append(f"  I-Interval:  {self.last_i_interval} ms")
        lines.append(f"  Interval:    {self.last_interval} ms")
        lines.append(f"  Payload:     {len(self.payload)} bytes")
        lines.append("=" * 60)
        return "\n".join(lines)


class JTT1078Server:
    """JT/T 1078 TCP server"""
    
    def __init__(self, port=6605):
        self.port = port
        self.running = False
        self.clients = {}  # client_addr -> client_info
        self.stats = {
            'total_packets': 0,
            'total_bytes': 0,
            'i_frames': 0,
            'p_frames': 0,
            'start_time': None
        }
    
    def start(self):
        """Start server"""
        self.running = True
        self.stats['start_time'] = time.time()
        
        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind(('0.0.0.0', self.port))
        server_sock.listen(5)
        
        print("=" * 60)
        print(f"🚀 JT/T 1078 Server Started")
        print("=" * 60)
        print(f"  Listening on port: {self.port}")
        print(f"  Time: {datetime.now()}")
        print("=" * 60)
        print("\n⏳ Waiting for connections...\n")
        
        # Stats thread
        stats_thread = threading.Thread(target=self.stats_monitor, daemon=True)
        stats_thread.start()
        
        try:
            while self.running:
                try:
                    client_sock, client_addr = server_sock.accept()
                    print(f"\n✅ New connection from {client_addr[0]}:{client_addr[1]}")
                    
                    # Handle client in new thread
                    client_thread = threading.Thread(
                        target=self.handle_client,
                        args=(client_sock, client_addr),
                        daemon=True
                    )
                    client_thread.start()
                    
                except KeyboardInterrupt:
                    break
        
        finally:
            server_sock.close()
            print("\n\n🛑 Server stopped")
            self.print_final_stats()
    
    def handle_client(self, client_sock, client_addr):
        """Handle individual client connection"""
        client_key = f"{client_addr[0]}:{client_addr[1]}"
        self.clients[client_key] = {
            'connected_at': datetime.now(),
            'packets': 0,
            'bytes': 0,
            'sim': None,
            'channel': None
        }
        
        buffer = b''
        
        try:
            while self.running:
                data = client_sock.recv(4096)
                if not data:
                    break
                
                buffer += data
                
                # Try to parse complete packets
                while len(buffer) >= 34:  # Minimum header size
                    try:
                        # Check header
                        header_flag = struct.unpack('>I', buffer[0:4])[0]
                        if header_flag != JTT1078_HEADER_FLAG:
                            print(f"❌ Invalid header from {client_key}, dropping buffer")
                            buffer = b''
                            break
                        
                        # Get data length
                        data_length = struct.unpack('>H', buffer[32:34])[0]
                        packet_size = 34 + data_length
                        
                        # Check if we have complete packet
                        if len(buffer) < packet_size:
                            break  # Wait for more data
                        
                        # Extract packet
                        packet_data = buffer[:packet_size]
                        buffer = buffer[packet_size:]
                        
                        # Parse and process
                        self.process_packet(packet_data, client_key)
                        
                    except Exception as e:
                        print(f"❌ Error parsing packet from {client_key}: {e}")
                        buffer = b''
                        break
        
        except Exception as e:
            print(f"❌ Error handling client {client_key}: {e}")
        
        finally:
            client_sock.close()
            print(f"\n❌ Client disconnected: {client_key}")
            del self.clients[client_key]
    
    def process_packet(self, packet_data, client_key):
        """Process received packet"""
        try:
            packet = JTT1078Packet(packet_data)
            
            # Update client info
            client = self.clients[client_key]
            if client['sim'] is None:
                client['sim'] = packet.sim
                client['channel'] = packet.channel
            
            client['packets'] += 1
            client['bytes'] += len(packet_data)
            
            # Update global stats
            self.stats['total_packets'] += 1
            self.stats['total_bytes'] += len(packet_data)
            
            if packet.data_type == 0x00:  # I-frame
                self.stats['i_frames'] += 1
                print(f"\n🔑 I-Frame #{packet.packet_seq} from {client['sim']} (channel {client['channel']})")
            elif packet.data_type == 0x01:  # P-frame
                self.stats['p_frames'] += 1
            
            # Verbose output for first few packets
            if client['packets'] <= 3:
                print(packet)
            
        except Exception as e:
            print(f"❌ Error processing packet: {e}")
    
    def stats_monitor(self):
        """Print periodic statistics"""
        while self.running:
            time.sleep(10)
            self.print_stats()
    
    def print_stats(self):
        """Print current statistics"""
        runtime = time.time() - self.stats['start_time']
        
        print("\n" + "=" * 60)
        print(f"📊 Statistics (Runtime: {runtime:.1f}s)")
        print("=" * 60)
        print(f"  Total Packets:  {self.stats['total_packets']}")
        print(f"  Total Bytes:    {self.stats['total_bytes'] / 1024 / 1024:.2f} MB")
        print(f"  I-frames:       {self.stats['i_frames']}")
        print(f"  P-frames:       {self.stats['p_frames']}")
        
        if self.stats['total_packets'] > 0:
            rate = self.stats['total_bytes'] / runtime / 1024  # KB/s
            print(f"  Data Rate:      {rate:.2f} KB/s")
        
        print(f"\n  Active Clients: {len(self.clients)}")
        for client_key, client_info in self.clients.items():
            print(f"    {client_key}: SIM={client_info['sim']}, "
                  f"CH={client_info['channel']}, "
                  f"Packets={client_info['packets']}")
        print("=" * 60)
    
    def print_final_stats(self):
        """Print final statistics on shutdown"""
        print("\n" + "=" * 60)
        print("📊 Final Statistics")
        print("=" * 60)
        print(f"  Total Packets:  {self.stats['total_packets']}")
        print(f"  Total Bytes:    {self.stats['total_bytes'] / 1024 / 1024:.2f} MB")
        print(f"  I-frames:       {self.stats['i_frames']}")
        print(f"  P-frames:       {self.stats['p_frames']}")
        print("=" * 60)


def main():
    port = 6605
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    
    server = JTT1078Server(port)
    
    try:
        server.start()
    except KeyboardInterrupt:
        print("\n\n⚠️  Interrupted by user")
        server.running = False


if __name__ == '__main__':
    main()
