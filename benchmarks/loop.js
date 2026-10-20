// benchmarks/loop.js — same tight loop, 5,000,000 iterations.
let total = 0;
let i = 0;
while (i < 5000000) {
    total = total + i;
    i = i + 1;
}
console.log(total);
