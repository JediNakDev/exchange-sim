// Read-only native records and copied delivery. Own browser only, no service lifecycle.
const {chromium}=require('/tmp/exsim-browser-tools/node_modules/playwright');
const fs=require('node:fs'), path=require('node:path'), crypto=require('node:crypto'), assert=require('node:assert/strict');
const dir=__dirname, root='/tmp/exsim-persona-review-75cf28f', base='http://127.0.0.1:45882';
const prior=JSON.parse(fs.readFileSync(path.join(dir,'../VALIDATION_FIRSTUSE_FINAL_RECHECK_2026-11-08/evidence.json')));
const out={artifact:{},pageErrors:[],responses:[],checks:[]};
const sha=b=>crypto.createHash('sha256').update(b).digest('hex');
function check(name,condition){out.checks.push({name,pass:!!condition});assert(condition,name);}
(async()=>{
const delivery=JSON.parse(fs.readFileSync(root+'/delivery.json')); let files=0;
for(const [p,e] of Object.entries(delivery.entries)){if(e.sha256){const b=fs.readFileSync(root+'/'+p);assert.equal(b.length,e.size,p);assert.equal(sha(b),e.sha256,p);files++;}}
out.artifact={...Object.fromEntries(Object.entries(delivery).filter(([k])=>k!=='entries')),verifiedFiles:files,deliverySha256:sha(fs.readFileSync(root+'/delivery.json')),apiSha256:sha(fs.readFileSync(root+'/api/exsim_api')),buildId:fs.readFileSync(root+'/frontend/.next/BUILD_ID','utf8').trim()};
check('declared source and build',delivery.api_revision==='360ad69529aa57b5e8856391812cc3d1d93c6896'&&delivery.web_revision==='75cf28fcdbee8e82bd71ba20264604ff9dfdac15'&&out.artifact.buildId==='efg6fmC6FKFd5gxu0E1Hb');
const browser=await chromium.launch({headless:true}); const context=await browser.newContext({viewport:{width:1280,height:800}});const page=await context.newPage();page.on('pageerror',e=>out.pageErrors.push(String(e)));page.on('response',r=>{if(r.status()>=400)out.responses.push({url:r.url(),status:r.status()});});
async function snap(name){await page.screenshot({path:dir+'/'+name+'.png',fullPage:true});fs.writeFileSync(dir+'/'+name+'.txt',(await page.locator('body').innerText()).split('\n').map(line=>line.trimEnd()).join('\n'));}
async function api(url){const r=await context.request.get(url);assert.equal(r.status(),200,url);return r.json();}
try{
await page.goto(base+'/runs/'+prior.run);await page.getByRole('heading',{name:'Parent-order execution',exact:true}).waitFor();
const assets=await page.locator('script[src]').evaluateAll(es=>es.map(e=>e.src).filter(s=>s.includes('/_next/static/')));out.artifact.servedAssets=[];
for(const url of assets){const r=await context.request.get(url);assert.equal(r.status(),200);const b=await r.body();const local=root+'/frontend/.next/static/'+new URL(url).pathname.split('/_next/static/')[1];assert.equal(sha(b),sha(fs.readFileSync(local)));out.artifact.servedAssets.push({url,sha256:sha(b),bytes:b.length});}
out.artifact.nativeRun=await api('http://127.0.0.1:45873/v1/runs/'+prior.run+'/manifest');
check('native manifest exact provenance',out.artifact.nativeRun.provenance.sourceRevision===delivery.api_revision&&out.artifact.nativeRun.provenance.executableSha256===out.artifact.apiSha256);
out.unavailableComparison=await api('http://127.0.0.1:45873/v1/comparisons?baselineExperimentId='+prior.baselineId+'&candidateExperimentId='+prior.failedExp.experimentId);
for(const seed of out.unavailableComparison.attemptLedger)for(const a of seed.attempts)if(['run_fc7e28b757f16efa','run_7ff458b7fbcbf284'].includes(a.runId))check(a.runId+' native null unavailable',a.windowEndTimeNs===null&&a.observationStatus==='unavailable');
await page.goto(base+'/compare?baseline='+prior.baselineId+'&candidate='+prior.failedExp.experimentId);await page.getByText('Inspect every planned seed and run attempt',{exact:true}).click();
out.ledger=await page.getByRole('table',{name:'Comparison attempt ledger'}).innerText();await snap('01-ledger');

for(const id of ['run_fc7e28b757f16efa','run_7ff458b7fbcbf284']){const row=page.getByRole('table',{name:'Comparison attempt ledger'}).locator('tbody tr').filter({hasText:id});check(id+' unavailable deadline', (await row.innerText()).includes('Deadline unavailable')&&!(await row.innerText()).includes('Not an execution window'));}
out.comparisons={};
for(const name of ['censor-comparison','width-comparison']){
 const old=JSON.parse(fs.readFileSync(path.join(dir,'../TRADING_BROWSER_FINAL_RECHECK_2026-11-08/'+name+'.json')));
 const b=old.baseline.experimentId,c=old.candidate.experimentId;
 const now=await api('http://127.0.0.1:45873/v1/comparisons?baselineExperimentId='+b+'&candidateExperimentId='+c);out.comparisons[name]={attemptLedger:now.attemptLedger,runSelection:now.runSelection};
 check(name+' selection and ledger unchanged',JSON.stringify(now.attemptLedger)===JSON.stringify(old.attemptLedger)&&now.runSelection===old.runSelection);
 await page.goto(base+'/compare?baseline='+b+'&candidate='+c);await page.getByText('Inspect every planned seed and run attempt',{exact:true}).click();
 out.comparisons[name].text=await page.getByRole('table',{name:'Comparison attempt ledger'}).innerText();
 for(const seed of now.attemptLedger)for(const a of seed.attempts){const text=await page.getByRole('table',{name:'Comparison attempt ledger'}).locator('tbody tr').filter({hasText:a.runId}).innerText();if(a.observationStatus==='not_applicable')check(a.runId+' actual MM not applicable',text.includes('Not an execution window'));if(a.windowEndTimeNs==='31000000000')check(a.runId+' exact deadline',text.includes('31 s'));if(a.observedThroughTimeNs==='5000000000')check(a.runId+' exact censored observation',text.includes('5 s / 31 s'));}
 await snap('02-'+name);
}
await page.goto(prior.pin.replace('45872','45882'));await page.getByText('Inspect every planned seed and run attempt',{exact:true}).click();out.pinnedLedger=await page.getByRole('table',{name:'Comparison attempt ledger'}).innerText();out.pinnedUrl=page.url();check('original pin retained',new URL(page.url()).searchParams.get('runSelection')===new URL(prior.pin).searchParams.get('runSelection'));check('old attempt remains selected in pin',(await page.getByRole('table',{name:'Comparison attempt ledger'}).locator('tbody tr').filter({hasText:prior.run}).innerText()).includes('Selected'));
async function geometry(){return page.evaluate(()=>({viewport:{width:innerWidth,height:innerHeight},document:document.documentElement.scrollWidth,cards:[...document.querySelectorAll('[data-slot="card"]')].map(e=>({text:e.innerText.slice(0,140),x:e.getBoundingClientRect().x,width:e.getBoundingClientRect().width,right:e.getBoundingClientRect().right})),tables:[...document.querySelectorAll('table')].map(t=>{let p=t.parentElement;return {headers:[...t.querySelectorAll('th')].map(e=>e.innerText),width:t.getBoundingClientRect().width,parentWidth:p.clientWidth,scrollWidth:p.scrollWidth,overflow:getComputedStyle(p).overflowX};})}));}
out.layouts={};
for(const [name,run,w,h] of [['execution-mobile',prior.run,390,844],['execution-desktop',prior.run,1440,900],['mm-mobile',prior.pagingRun3.runId,390,844],['mm-desktop',prior.pagingRun3.runId,1440,900]]){
 await page.setViewportSize({width:w,height:h});await page.goto(base+'/runs/'+run);await page.getByRole('tab',{name:'Summary',exact:true}).waitFor();await page.getByRole('heading',{name:name.startsWith('execution')?'Parent-order execution':'Outbound request diagnostics',exact:true}).waitFor();out.layouts[name]=await geometry();
 if(w===390){const title=name.startsWith('execution')?'Parent-order execution':'Outbound request diagnostics';await page.getByRole('heading',{name:title,exact:true}).evaluate(e=>window.scrollTo(0,window.scrollY+e.getBoundingClientRect().top-180));await page.screenshot({path:dir+'/03-'+name+'-primary.png'});}
check(name+' primary cards fit viewport',out.layouts[name].cards.every(c=>c.x>=0&&c.right<=w));check(name+' document constrained',out.layouts[name].document===w);
 out.layouts[name].scrollChecks=await page.evaluate(()=>[...document.querySelectorAll('table')].filter(t=>t.parentElement.scrollWidth>t.parentElement.clientWidth).map(t=>{const p=t.parentElement;p.scrollLeft=p.scrollWidth;const cell=t.querySelector('tr th:last-child');return {headers:[...t.querySelectorAll('th')].map(x=>x.innerText),client:p.clientWidth,scroll:p.scrollWidth,scrollLeft:p.scrollLeft,lastRight:cell.getBoundingClientRect().right,parentRight:p.getBoundingClientRect().right,visible:cell.getBoundingClientRect().right<=p.getBoundingClientRect().right+1&&cell.getBoundingClientRect().left>=p.getBoundingClientRect().left};}));
 for(const t of out.layouts[name].scrollChecks)check(name+' final table column reachable',t.scrollLeft>0&&t.visible);await snap('03-'+name);
}
await page.setViewportSize({width:1280,height:800});await page.goto(base+'/runs/'+prior.pagingRun3.runId+'/inspect');
const fills=page.getByRole('table',{name:'Recorded fills'}), region=page.getByRole('region',{name:'Selected order explanation'});
const focus=()=>page.evaluate(()=>({tag:document.activeElement.tagName,role:document.activeElement.getAttribute('role'),label:document.activeElement.getAttribute('aria-label'),text:document.activeElement.innerText?.slice(0,250)}));
const status=()=>page.getByRole('status').allTextContents();
out.focus={};let row=fills.locator('tbody tr').first();await row.focus();await page.keyboard.press('Enter');await page.getByRole('button',{name:'Read explanation',exact:true}).waitFor();out.focus.selected={focus:await focus(),status:await status(),row:await row.innerText()};
check('select first fill keyboard retains row focus',await row.evaluate(e=>e===document.activeElement));
await page.keyboard.press('Tab');check('Tab reaches Read explanation',(await focus()).text==='Read explanation');await page.keyboard.press('Enter');out.focus.read=await focus();check('Read focuses explanation',out.focus.read.label==='Selected order explanation');
await region.getByRole('button',{name:'Return to selected row',exact:true}).focus();await page.keyboard.press('Enter');check('Return focuses originating row',await row.evaluate(e=>e===document.activeElement));
await snap('04-selected-fill');
await page.getByRole('button',{name:'Next fills',exact:true}).click();await page.getByRole('button',{name:'Previous fills',exact:true}).waitFor();await fills.locator('tbody tr').first().filter({hasNotText:'0.062980183 s'}).waitFor();out.focus.next={status:await status(),label:await region.getByRole('button').first().innerText(),explanation:await region.innerText()};check('selected fill explanation retained after paging',out.focus.next.explanation.includes('Selected fill: order 7, event 45, match 4'));
check('paging truthful fallback',out.focus.next.label==='Return to fills and own orders'&&out.focus.next.status.join(' ').includes('Return goes to fills and own orders')&&!out.focus.next.status.join(' ').includes('available on the selected row'));
await region.getByRole('button',{name:'Return to fills and own orders',exact:true}).click();out.focus.next.returnFocus=await focus();check('paging return focuses list',out.focus.next.returnFocus.label==='Fills and own orders');await snap('05-paging-fallback');
await page.getByRole('button',{name:'Previous fills',exact:true}).click();await fills.locator('tbody tr').first().filter({hasText:'0.062980183 s'}).waitFor();out.focus.previous={status:await status(),label:await region.getByRole('button').first().innerText()};
check('Previous preserves truthful fallback',out.focus.previous.label==='Return to fills and own orders');
row=fills.locator('tbody tr').first();await row.focus();await page.keyboard.press('Enter');await page.getByRole('button',{name:'Read explanation',exact:true}).waitFor();check('reselection restores row return',await region.getByRole('button',{name:'Return to selected row',exact:true}).count()===1);
await page.getByRole('tab',{name:'Own orders',exact:true}).click();out.focus.tabSwitch={status:await status(),label:await region.getByRole('button').first().innerText(),explanation:await region.innerText()};check('artifact switch truthful fallback',out.focus.tabSwitch.label==='Return to fills and own orders'&&out.focus.tabSwitch.status.join(' ').includes('Return goes to fills and own orders'));await region.getByRole('button',{name:'Return to fills and own orders',exact:true}).click();check('artifact switch return focuses list',(await focus()).label==='Fills and own orders');
await page.getByRole('tab',{name:'Fills',exact:true}).click();await fills.locator('tbody tr').first().waitFor();row=fills.locator('tbody tr').first();await row.focus();await page.keyboard.press('Enter');
await page.getByRole('button',{name:'Next event',exact:true}).focus();await page.keyboard.press('Enter');out.focus.step=await focus();check('event step retains button focus',out.focus.step.text===''&&await page.getByRole('button',{name:'Next event',exact:true}).evaluate(e=>e===document.activeElement));
await page.goto(base+'/runs/'+prior.pagingRun3.runId+'/inspect?order=7');await region.getByRole('button',{name:'Return to fills and own orders',exact:true}).waitFor();out.focus.direct={status:await status(),explanation:await region.innerText()};await region.getByRole('button',{name:'Return to fills and own orders',exact:true}).click();out.focus.direct.returnFocus=await focus();check('direct order link return list',out.focus.direct.returnFocus.label==='Fills and own orders');await snap('06-direct-order');
check('no page errors',out.pageErrors.length===0);console.log(JSON.stringify({checks:out.checks,layouts:Object.fromEntries(Object.entries(out.layouts).map(([k,v])=>[k,v.document])),pageErrors:out.pageErrors},null,2));

}finally{fs.writeFileSync(dir+'/evidence.json',JSON.stringify(out,null,2)+'\n');await context.close();await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
