// Facilitator-only preparation, not human first-use results. Existing local Playwright.
const {chromium}=require('/tmp/exsim-browser-tools/node_modules/playwright');
const fs=require('node:fs');
const source=process.env.HUMAN_REFERENCE_OUTPUT || '/tmp/exsim-human-reference-75cf28f-out-2';
const receipt=JSON.parse(fs.readFileSync(source+'/receipt.json'));
const run=receipt.controlRun.runId;
const base='http://127.0.0.1:45992';
const api='http://127.0.0.1:45993/v1';
(async()=>{
 const browser=await chromium.launch({headless:true});
 const context=await browser.newContext({viewport:{width:1440,height:900}});
 const page=await context.newPage();
 const result={human_result:'NOT RUN',pageErrors:[],controlRun:run,stepRequests:[]};
 page.on('pageerror', e=>result.pageErrors.push(String(e)));
 try {
  await page.goto(base+'/experiments/new?intent=execution');
  const unit=page.getByRole('combobox',{name:'Display unit for Parent start',exact:true});
  result.timing={};
  for(const choice of ['ns','ms','s']){
   await unit.click();await page.getByRole('option',{name:choice,exact:true}).click();
   result.timing[choice]=await page.getByRole('textbox',{name:/^Parent start/}).inputValue();
  }
  if(result.timing.ns!=='1000000000'||result.timing.ms!=='1000'||result.timing.s!=='1')throw Error('timing conversion');
  await page.goto(base+'/runs/'+run);
  await page.getByRole('button',{name:'Step events',exact:true}).waitFor();
  let interrupt=true;
  await page.route('**/api/exsim/v1/runs/'+run+'/step',async route=>{
   result.stepRequests.push({key:route.request().headers()['idempotency-key'],body:route.request().postDataJSON()});
   const response=await route.fetch();
   if(interrupt){interrupt=false;await route.abort('connectionreset');}
   else await route.fulfill({response});
  });
  await page.getByRole('button',{name:'Step events',exact:true}).click();
  await page.getByText('Control request rejected',{exact:true}).waitFor();
  const before=await (await fetch(api+'/runs/'+run)).json();
  await page.screenshot({path:source+'/control-interrupted.png',fullPage:true});
  await page.getByRole('button',{name:'Step events',exact:true}).click();
  await page.getByText('Control request rejected',{exact:true}).waitFor({state:'hidden'});
  const after=await (await fetch(api+'/runs/'+run)).json();
  result.afterUncertainStep=before;result.afterRetry=after;
  if(result.stepRequests.length!==2||result.stepRequests[0].key!==result.stepRequests[1].key)throw Error('retry changed idempotency identity');
  if(before.progress.processedCanonicalEventCount==='0'||before.progress.processedCanonicalEventCount!==after.progress.processedCanonicalEventCount)throw Error('duplicate step');
  await page.getByRole('button',{name:'Resume',exact:true}).click();
  await page.getByText('Run complete.',{exact:false}).waitFor();
  result.afterResume=await (await fetch(api+'/runs/'+run)).json();
  if(result.afterResume.state!=='completed')throw Error('resume failed');
  await page.screenshot({path:source+'/control-resumed.png',fullPage:true});
  result.result='PASS';
 } finally {
  fs.writeFileSync(source+'/browser.json',JSON.stringify(result,null,2)+'\n');
  await context.close();await browser.close();
 }
})().catch(e=>{console.error(e);process.exitCode=1;});
