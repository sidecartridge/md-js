/**
 * math.js — basic maths functions example for MD/JS Code
 *
 * Demonstrates arithmetic, rounding and clamping using JS Math methods.
 * Run with any function name and appropriate numeric parameters.
 */

function add(a, b) {
  return a + b;
}

function subtract(a, b) {
  return a - b;
}

function multiply(a, b) {
  return a * b;
}

function divide(a, b) {
  if (b === 0) return 'Error: division by zero';
  return a / b;
}

function modulo(a, b) {
  if (b === 0) return 'Error: division by zero';
  return a % b;
}

function power(base, exp) {
  return Math.pow(base, exp);
}

function abs(n) {
  return Math.abs(n);
}

function min(a, b) {
  return Math.min(a, b);
}

function max(a, b) {
  return Math.max(a, b);
}

function clamp(n, lo, hi) {
  return Math.min(Math.max(n, lo), hi);
}
