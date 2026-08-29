import socket, struct, sys, time

# name, code -- framebuffer encodings first, then pseudo-encodings
ENCS = [
 ("raw",0), ("copyrect",1), ("rre",2), ("corre",4), ("hextile",5),
 ("zlib",6), ("tight",7), ("zlibhex",8), ("trle",15), ("zrle",16),
 ("zywrle",17), ("tightpng",-260), ("h264",50), ("open-h264",0x48323634),
]
PSEUDO = [
 ("cursor",-239), ("xcursor",-240), ("cursor-with-alpha",-314),
 ("desktopsize",-223), ("extended-desktopsize",-308), ("desktopname",-307),
 ("lastrect",-224), ("fence",-312), ("continuous-updates",-313),
 ("qemu-ext-key",-258), ("qemu-led",-261), ("extended-clipboard",0xc0a1e5ce),
 ("vmware-cursor",0x574d5664), ("pts",-1000),
]
NAME = {c:n for n,c in ENCS+PSEUDO}

def probe(port, code, pseudo=False, timeout=6.0):
    s=socket.create_connection(("127.0.0.1",port),timeout=timeout); s.settimeout(timeout)
    try:
        s.recv(12); s.sendall(b"RFB 003.008\n")
        t=s.recv(s.recv(1)[0]); s.sendall(bytes([1 if 1 in t else t[0]]))
        if struct.unpack(">I",s.recv(4))[0]!=0: return "authfail"
        s.sendall(b"\x01")
        h=s.recv(24); w,hh=struct.unpack(">HH",h[:4])
        s.recv(struct.unpack(">I",s.recv(4))[0])
        # advertise ONLY the encoding under test (plus raw for pseudo probes,
        # since a pseudo-encoding cannot carry pixels by itself)
        codes=[code]+([0] if pseudo else [])
        s.sendall(struct.pack(">BBH",2,0,len(codes))
                  + b"".join(struct.pack(">i",c) for c in codes))
        time.sleep(0.4)
        s.sendall(struct.pack(">BBHHHH",3,0,0,0,64,64))
        buf=bytearray()
        def take(n):
            while len(buf)<n:
                d=s.recv(65536)
                if not d: raise EOFError
                buf.extend(d)
            r=bytes(buf[:n]); del buf[:n]; return r
        seen=set()
        t0=time.time()
        while time.time()-t0 < timeout-1:
            m=take(1)[0]
            if m!=0:
                if m==1:
                    take(3); n=struct.unpack(">H",take(2))[0]; take(n*6)
                elif m==2: pass
                elif m==3:
                    take(3); n=struct.unpack(">I",take(4))[0]; take(n)
                else: return "msg%d"%m
                continue
            take(1); nr=struct.unpack(">H",take(2))[0]
            for _ in range(nr):
                x,y,rw,rh,enc=struct.unpack(">HHHHi",take(12))
                seen.add(enc)
                if enc==0: take(rw*rh*4)
                elif enc==1: take(4)
                else: return ",".join(sorted(NAME.get(e,str(e)) for e in seen))
            return ",".join(sorted(NAME.get(e,str(e)) for e in seen))
        return "timeout"
    except Exception as e:
        return type(e).__name__
    finally:
        s.close()

port=int(sys.argv[1]); label=sys.argv[2]
print("=== %s (port %d) ===" % (label, port))
print("  -- framebuffer encodings (advertised alone; 'raw' back means unsupported) --")
for n,c in ENCS:
    if c==0: continue
    print("    %-22s -> %s" % (n, probe(port,c)))
print("  -- pseudo-encodings (advertised with raw) --")
for n,c in PSEUDO:
    r=probe(port,c,pseudo=True)
    used = "sent" if n in r else "not sent"
    print("    %-22s -> %-28s %s" % (n, r, used))
