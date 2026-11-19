from pathlib import Path
import argparse,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
root=Path(__file__).resolve().parent;out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
results=[]
for test in json.loads((root/'host-checks.json').read_text()):
 name=test['name'];exe=out/name;cwd=root/test['cwd']
 flags=['gcc','-O1','-g','-std=gnu11','-Wall','-Wextra','-I.','-D_GNU_SOURCE','-DASCENT_HOSTED','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-ffile-prefix-map='+str(root)+'=/candidate']
 build=subprocess.run([*flags,*test['sources'],'-lm','-o',str(exe)],cwd=cwd,capture_output=True,timeout=90)
 (out/(name+'.build.log')).write_bytes(build.stdout+build.stderr)
 result={'suite':name,'build':build.returncode,'run':None}
 if build.returncode==0:
  run=subprocess.run([str(exe)],cwd=cwd,capture_output=True,timeout=90)
  (out/(name+'.run.log')).write_bytes(run.stdout+run.stderr);result['run']=run.returncode
 results.append(result)
(out/'RESULTS.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results));raise SystemExit(0 if all(x['build']==0 and x['run']==0 for x in results) else 1)
