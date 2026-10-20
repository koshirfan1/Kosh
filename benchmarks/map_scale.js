// benchmarks/map_scale.js — same, for comparison.
const m = {};
let i = 0;
while (i < 4000) {
    m["key" + i] = i;
    i = i + 1;
}
console.log(Object.keys(m).length);
