// benchmarks/string_concat.js — same repeated concatenation, 20,000 iterations.
let s = "";
let i = 0;
while (i < 20000) {
    s = s + "x";
    i = i + 1;
}
console.log(s.length);
