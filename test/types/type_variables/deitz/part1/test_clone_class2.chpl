class foo {
  var x;
  proc print() {
    writeln(x);
  }
}

var f = new foo(2);

f.print();

var f2 = new foo(3.2);

f2.print();
