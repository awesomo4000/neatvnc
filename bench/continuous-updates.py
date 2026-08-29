import socket, struct, sys, time
port=int(sys.argv[1]); label=sys.argv[2]; use_cu=int(sys.argv[3])
s=socket.create_connection(("127.0.0.1",port),timeout=25); s.settimeout(12)
s.recv(12); s.sendall(b"RFB 003.008\n")
t=s.recv(s.recv(1)[0]); s.sendall(bytes([1 if 1 in t else t[0]])); s.recv(4); s.sendall(b"\x01")
h=s.recv(24); w,hh=struct.unpack(">HH",h[:4]); bpp=h[4]
s.recv(struct.unpack(">I",s.recv(4))[0])
encs=[0,1]+([-313] if use_cu else [])
s.sendall(struct.pack(">BBH",2,0,len(encs))+b"".join(struct.pack(">i",e) for e in encs))
time.sleep(0.5)
buf=bytearray()
def take(n):
    while len(buf)<n:
        d=s.recv(65536)
        if not d: raise EOFError
        buf.extend(d)
    r=bytes(buf[:n]); del buf[:n]; return r
if use_cu:
    # EnableContinuousUpdates: type 150, enable=1, x, y, w, h
    s.sendall(struct.pack(">BBHHHH",150,1,0,0,w,hh))
else:
    s.sendall(struct.pack(">BBHHHH",3,1,0,0,w,hh))
n=0; total=0; copyrects=0; t0=time.time()
try:
    while time.time()-t0 < 8:
        m=take(1)[0]
        if m==150:
            continue
        if m!=0:
            if m==1: take(3); k=struct.unpack(">H",take(2))[0]; take(k*6)
            elif m==2: pass
            elif m==3: take(3); k=struct.unpack(">I",take(4))[0]; take(k)
            elif m==248: take(8)
            continue
        take(1); nr=struct.unpack(">H",take(2))[0]
        for _ in range(nr):
            x,y,rw,rh,enc=struct.unpack(">HHHHi",take(12))
            if enc==0: take(rw*rh*(bpp//8)); total+=rw*rh*(bpp//8)
            elif enc==1: take(4); copyrects+=1; total+=4
        n+=1
        if not use_cu:
            s.sendall(struct.pack(">BBHHHH",3,1,0,0,w,hh))
except Exception as e:
    pass
el=time.time()-t0
print("  %-34s %5.1f upd/s  %8.0f KiB  copyrect_rects=%d" %
      (label, n/el, total/1024, copyrects))
s.close()
