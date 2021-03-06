use MRA;
use MadAnalytics;

proc main() {
    var npt = 10;

    writeln("Mad Chapel -- Concurrent Compression Test\n");

    var fcn  : [1..4] AFcn = ((new Fn_Test1():AFcn),  (new Fn_Test2():AFcn),  (new Fn_Test3():AFcn), (new Fn_Unity():AFcn));
    var dfcn : [1..4] AFcn = ((new Fn_dTest1():AFcn), (new Fn_dTest2():AFcn), (new Fn_dTest3():AFcn), (new Fn_dUnity():AFcn));

    for i in fcn.domain {
        writeln("** Testing function ", i);
        var F = new Function(k=5, thresh=1e-5, f=fcn[i]);
        writeln("F", i, ".norm2() = ", F.norm2());

        writeln("Compressing F", i, " ...");
        F.compress();
        
        F.summarize();

        if i < fcn.domain.dim(1).high then
            writeln("\n======================================================================\n");
        delete F;
    }

    for (f,d) in zip(fcn,dfcn) {
      delete f;
      delete d;
    }
}
