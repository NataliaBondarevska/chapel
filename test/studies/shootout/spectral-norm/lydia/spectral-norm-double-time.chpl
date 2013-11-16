/*
 * The Computer Language Benchmarks Game
 * http://shootout.alioth.debian.org/
 *
 * Original C contributed by Sebastien Loisel
 * Conversion to Chapel by Albert Sidelnik
 * Updated by Lydia Duncan
 */

config const NUM = 500 : int(64);


/* Return: 1.0 / (i + j) * (i + j +1) / 2 + i + 1; */
inline proc eval_A(i, j) : real
{
  /*
   * 1.0 / (i + j) * (i + j +1) / 2 + i + 1;
   * n * (n+1) is even number. Therefore, just (>> 1) for (/2)
   */
  const d = (((i + j) * (i + j + 1)) >> 1) + i + 1;
  return 1.0 / d;
}

/* This version behaves similarly to version 4 of the C implementation,
 * in that it performs two additions at a time instead of one per
 * process.  This drastically improves performance on machines with a
 * smaller number of cores, but does not show an improvement on our largest
 * testing machine
 */
inline proc eval_A_times_u(U : [] real, inRange, Au : [] real)
{
  forall i in {0..#inRange} do { 
    Au(i) = + reduce [j in 0..#inRange by 2] ((U(j) * eval_A(i,j)) + (U(j+1) * eval_A(i, j+1)));
    if (inRange % 2 == 1) {
      Au(i) = Au(i) + (U(inRange - 1) * eval_A(i, inRange - 1));
    }
  }
}

inline proc eval_At_times_u(U : [] real, inRange, Au : [] real)
{
  forall i in {0..#inRange} do {
    Au(i) = + reduce [j in 0..#inRange by 2] ((U(j) * eval_A(j,i)) + (U(j+1) * eval_A(j+1, i)));
    if (inRange % 2 == 1) {
      Au(i) = Au(i) + (U(inRange - 1) * eval_A(inRange - 1, i));
    }
  }
}

inline proc eval_AtA_times_u(u,AtAu,v : [] real, inRange)
{
     eval_A_times_u(u, inRange, v);
     eval_At_times_u(v, inRange, AtAu);
}

proc spectral_game(N) : real
{
  var tmp, U, V : [0..#N] real;

  U = 1.0;

  for 1..10 do {
    eval_AtA_times_u(U,V,tmp,N);
    eval_AtA_times_u(V,U,tmp,N);
  }

  const vv = + reduce [v in V] (v * v);
  const vBv = + reduce [(u,v) in zip(U,V)] (u * v);

  return sqrt(vBv/vv);
}

proc main() {
  writeln(spectral_game(NUM), new iostyle(precision=10));
}