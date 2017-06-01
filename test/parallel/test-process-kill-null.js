'use strict';
const common = require('../common');
const assert = require('assert');
const spawn = require('child_process').spawn;

const cat = common.spawnCat();
let called;

assert.ok(process.kill(cat.pid, 0));

cat.on('exit', function() {
  console.log("on exit");
  assert.throws(function() {
    process.kill(cat.pid, 0);
  }, Error);
});

cat.stdout.on('data', function() {
  called = true;
  process.kill(cat.pid, 'SIGKILL');
});

// EPIPE when null sig fails
cat.stdin.write('test');

process.on('exit', function() {
  console.log("on process exit");
  //assert.ok(called);
});
