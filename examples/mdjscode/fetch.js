/**
 * fetch.js — basic fetch() example for MD/JS Code
 *
 * Fetches a JSON resource over HTTP and returns a field from it.
 * Run with function name "main" and no parameters.
 */

async function main() {
  const response = await fetch("http://httpbin.org/json");

  if (!response.ok) {
    return { error: "fetch failed" };
  }

  const data = await response.json();
  return data;
}
