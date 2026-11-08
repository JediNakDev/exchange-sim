// Read-only review of existing, review-owned native records. No server or install.
const {chromium} = require('/tmp/exsim-browser-tools/node_modules/playwright');
const fs = require('node:fs');
const e = JSON.parse(fs.readFileSync(__dirname + '/evidence.json'));
(async () => {
  const browser = await chromium.launch({headless:true});
  const context = await browser.newContext({viewport:{width:390,height:844}});
  const page = await context.newPage();
  const observations = {};
  try {
    await page.goto('http://127.0.0.1:45872/runs/' + e.run);
    await page.getByRole('heading',{name:'Parent-order execution',exact:true}).waitFor();
    observations.runWidth = await page.evaluate(() => ({viewport:String(innerWidth),document:String(document.documentElement.scrollWidth)}));
    await page.goto('http://127.0.0.1:45872/compare?baseline='+e.baselineId+'&candidate='+e.failedExp.experimentId);
    await page.getByText('Inspect every planned seed and run attempt',{exact:true}).click();
    observations.ledger = await page.getByRole('table',{name:'Comparison attempt ledger'}).innerText();
    await page.goto('http://127.0.0.1:45872/runs/'+e.pagingRun3.runId+'/inspect');
    const row = page.getByRole('table',{name:'Recorded fills'}).locator('tbody tr').first();
    await row.focus(); await page.keyboard.press('Enter');
    await page.getByRole('button',{name:'Read explanation',exact:true}).waitFor();
    await page.getByRole('button',{name:'Next fills',exact:true}).click();
    observations.afterPaging = await page.getByRole('status').allTextContents();
    observations.returnLabel = await page.getByRole('region',{name:'Selected order explanation'}).getByRole('button').allTextContents();
    await page.getByRole('button',{name:'Return to selected row',exact:true}).click();
    observations.returnFocus = await page.evaluate(() => document.activeElement.getAttribute('aria-label'));
    console.log(JSON.stringify(observations,null,2));
  } finally {await context.close();await browser.close();}
})().catch(error => {console.error(error);process.exitCode=1;});
