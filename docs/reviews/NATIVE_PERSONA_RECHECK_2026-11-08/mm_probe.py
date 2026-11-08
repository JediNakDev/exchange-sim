"""Read-only probes of review-owned scratch runs; requires original API build."""
import sys,pathlib,json,urllib.request
base,root=sys.argv[1:];root=pathlib.Path(root);out=[]
def get(path):
 with urllib.request.urlopen(base+'/v1'+path) as r:v=json.load(r)
 out.append(dict(path=path,result=v));return v
exps=get('/experiments?limit=1000')['items'];slow=next(x for x in exps if x['name']=='NATIVE-RECHECK slow');run=get('/runs?experimentId='+slow['experimentId'])['items'][0]
l=get('/runs/'+run['runId']+'/orders/7/lifecycle');events=get('/runs/'+run['runId']+'/events?fromEventPosition=48&toEventPosition=105&limit=1000')
(root/'mm-original.json').write_text(json.dumps(out,indent=2));print(json.dumps({'lifecycle':l,'events':events},indent=2))
