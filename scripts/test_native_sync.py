#!/usr/bin/env python3
"""Run native TCP/CRDT clients against an isolated production Littlebear process."""
import argparse, json, re, select, subprocess, tempfile, time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('server');p.add_argument('clients',nargs='+');p.add_argument('--output')
a=p.parse_args();report={'status':'pass','clients':[]}
with tempfile.TemporaryDirectory(prefix='datafriend-native-sync-') as directory:
 server=subprocess.Popen([str(Path(a.server).resolve()),'--port','0','--values',directory+'/values'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  deadline=time.monotonic()+20;port=None
  while port is None:
   remaining=deadline-time.monotonic()
   if remaining<=0 or not select.select([server.stdout],[],[],remaining)[0]:raise RuntimeError('Server startup timed out')
   line=server.stdout.readline()
   if not line:raise RuntimeError('Server exited during startup')
   match=re.search(r'Littlebear TCP listening [^:]+:(\d+)',line)
   if match:port=match.group(1)
  for client in a.clients:
   result=subprocess.run([str(Path(client).resolve()),'127.0.0.1',port],capture_output=True,text=True,timeout=120)
   if result.returncode:raise RuntimeError(result.stdout+result.stderr)
   report['clients'].append({'binary':client,'result':result.stdout.strip()})
 finally:
  server.terminate()
  try:server.communicate(timeout=10)
  except subprocess.TimeoutExpired:server.kill();server.communicate()
text=json.dumps(report,indent=2)+'\n'
if a.output:Path(a.output).write_text(text)
print(text,end='')
