use myrecord;

proc foo(in rec: R) {
  rec.verify();
  rec.increment();
  rec.verify();
}

proc myfunction() {

  var myvar: R;
  myvar.init(x = 20);

  myvar.verify();
  assert(myvar.x == 20);

  foo(myvar);

  myvar.verify();
  assert(myvar.x == 20);
}

myfunction();

verify();
