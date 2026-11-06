import subprocess as sp, tempfile, pathlib, socket, time, os
binary=str(pathlib.Path(__file__).resolve().parents[1] / 'build/relay')
t=pathlib.Path(tempfile.mkdtemp(prefix='relay-test-')); key=t/'key'; key.write_bytes(os.urandom(32)); wrong=t/'wrong';wrong.write_bytes(os.urandom(32))
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return str(s.getsockname()[1])
def start(*args):
 p=sp.Popen([binary,*map(str,args)],stdout=sp.PIPE,stderr=sp.PIPE,text=True)
 line=p.stdout.readline()
 assert 'Listening' in line,(line,p.communicate())
 return p
def finish(p, expected=0):
 o,e=p.communicate(timeout=10);assert p.returncode==expected,(p.returncode,o,e)
for name,data in [('small',b'hello\n'),('large',os.urandom(250000)),('empty',b''),('missing',None)]:
 a,b,c=port(),port(),port();src=t/name;out=t/(name+'.out')
 if data is not None:src.write_bytes(data)
 r=start('receiver',out,'127.0.0.1',c);d=start('decoder','127.0.0.1',b,'127.0.0.1',c,key);e=start('encoder','127.0.0.1',a,'127.0.0.1',b,key)
 sp.run([binary,'sender',str(src),'127.0.0.1',a],check=True,capture_output=True)
 for p in (e,d,r):finish(p)
 assert out.read_bytes()==src.read_bytes();assert not pathlib.Path(str(out)+'.part').exists()
 print('PASS',name)
# Wrong key: decoder must reject and never contact receiver.
a,b,c=port(),port(),port();src=t/'small'
d=start('decoder','127.0.0.1',b,'127.0.0.1',c,wrong);e=start('encoder','127.0.0.1',a,'127.0.0.1',b,key)
sp.run([binary,'sender',str(src),'127.0.0.1',a],capture_output=True)
e.communicate(timeout=10);finish(d,1);print('PASS wrong key')
# Interrupted encrypted stream.
b=port();d=start('decoder','127.0.0.1',b,'127.0.0.1',port(),key)
with socket.create_connection(('127.0.0.1',int(b))) as s:s.sendall(b'12345678\x00\x00')
finish(d,1);print('PASS truncated encrypted stream')
# Interrupted final local link preserves previous output and removes .part.
c=port();out=t/'preserve';out.write_bytes(b'previous');r=start('receiver',out,'127.0.0.1',c)
with socket.create_connection(('127.0.0.1',int(c))) as s:s.sendall(b'\x00\x00\x00\x03abc')
finish(r,1);assert out.read_bytes()==b'previous';assert not pathlib.Path(str(out)+'.part').exists();print('PASS truncated local stream')
