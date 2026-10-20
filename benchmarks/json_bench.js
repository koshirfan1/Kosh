// benchmarks/json_bench.js — same encode+decode round-trip, 50,000 iterations.
const obj = {name: "khan", version: 1, tags: ["a", "b", "c", "d", "e"], nested: {x: 1, y: 2, z: 3}};
let i = 0;
while (i < 50000) {
    const s = JSON.stringify(obj);
    const d = JSON.parse(s);
    i = i + 1;
}
console.log("done");
