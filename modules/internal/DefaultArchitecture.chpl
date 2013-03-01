// DefaultArchitecture.chpl
//
// Provides a default architectural description.
// This architectural description is backward compatible with
// the architecture implicitly provided by releases 1.6 and preceding.
//
pragma "no use ChapelStandard"
module DefaultArchitecture {

  use ChapelLocale;
  use DefaultRectangular;
  use ChapelNumLocales;
  use RootLocale;
  use Sys;

  const emptyLocaleSpace: domain(1) = {1..0};
  const emptyLocales: [emptyLocaleSpace] locale;

  // We would really like a class-static storage class.(C++ nomenclature)
  var doneCreatingLocales: bool = false;

  //
  // A concrete class representing the nodes in this architecture.
  //
  class DefaultNode : locale {
    const callStackSize: int;
    const _node_id : int;
    const local_name : string;

    // This constructor must be invoked "on" the node
    // that it is intended to represent.  This trick is used
    // to establish the equivalence the "locale" field of the locale object
    // and the node ID portion of any wide pointer referring to it.
    proc DefaultNode() {
      if doneCreatingLocales {
        halt("locales cannot be created");
      }
      init;
    }

    proc DefaultNode(parent_loc : locale) {
      parent = parent_loc;
      init;
    }

    // chpl_nodeID is the node ID associated with the running image.
    proc chpl_id() return _node_id;
    proc chpl_name() return local_name;


    proc readWriteThis(f) {
      // Most classes will define it like this:
//      f <~> name;
      // but here it is defined thus for backward compatibility.
      f <~> new ioLiteral("LOCALE") <~> _node_id;
    }
  
    proc getChildSpace() return emptyLocaleSpace;
  
    proc getChildCount() return 0;
  
    iter getChildIndices() : int {
      for idx in emptyLocaleSpace do
        yield idx;
    }
  
    proc getChild(idx:int) : locale {
      // This is a temporary implementation, where an index of zero is forced
      // to mean "here".  A better solution is on the way.
      // TODO: Run index lookup through rootLocale.findLocale(lid:chpl_localeID_t);
      if idx == 0 then return this;
      else
        if boundsChecking then
          halt("requesting a child from a DefaultNode locale");
      return nil;
    }
  
    iter getChldren() : locale  {
      for loc in emptyLocales do
        yield loc;
    }

    proc getChildArray() {
      return emptyLocales;
    }

    // Part of the public interface required by the compiler
    // These are dynamically dispatched, so cannot be inlined.
    proc taskInit() {}
    proc taskExit() {}

    //------------------------------------------------------------------------{
    //- Implementation (private)
    //-
    proc init {
      _node_id = __primitive("chpl_nodeID");

      // chpl_nodeName is defined in chplsys.c.
      // It supplies a node name obtained by running uname(3) on the current node.
      // For this reason (as well), the constructor (or at least this init method)
      // must be run on the node it is intended to describe.
//      extern proc getenv(s:string) : string;
      var comm, spawnfn : string;
      extern proc chpl_nodeName() : string;
      // sys_getenv returns zero on success.
      if sys_getenv("CHPL_COMM", comm) == 0 && comm == "gasnet" &&
        sys_getenv("GASNET_SPAWNFN", spawnfn) == 0 && spawnfn == "L"
      then local_name = chpl_nodeName() + "-" + _node_id : string;
      else local_name = chpl_nodeName();

      extern proc chpl_task_getCallStackSize(): int;
      callStackSize = chpl_task_getCallStackSize();

      extern proc chpl_numCoresOnThisLocale(): int;
      numCores = chpl_numCoresOnThisLocale();
    }
    //------------------------------------------------------------------------}
  }
  
  //
  // An instance of this class is the default contents 'rootLocale'.
  // 
  // In the current implementation a platform-specific architectural description
  // may overwrite this instance or any of its children to establish a more customized
  // representation of the system resources.
  //
  class DefaultRootLocale : RootLocale {

    // Would like to make myLocaleSpace distributed with one index per node.
    const myLocaleSpace: domain(1) = {0..numLocales-1};
    const myLocales: [myLocaleSpace] locale;
  
    proc DefaultRootLocale()
    {
      parent = nil;
      numCores = 0;

      // We cannot use a forall here because the default leader iterator will
      // access 'Locales' and 'here', which are not yet initialized.
      for locIdx in myLocaleSpace do
        // Would like to call addChild here, but for some reason it does not work. <hilde>
        on __primitive("chpl_on_locale_num", locIdx)
        {
          const node = new DefaultNode(this);

          // The "private" implementation of _here is being replaced by
          // a task-private one.  So this will eventually go away:
          _here = node;

          myLocales[locIdx] = node;
          numCores += node.numCores;
        }

      doneCreatingLocales = true;

      // Sanity check.
      // Ensure that the locale representing node x has a nodeID of x.
      // At the top level, this lets us look up the locale representing the node on which
      // an object o lives by executing
      //  myLocales[__primitive("_wide_get_node", o)].
      for locIdx in myLocaleSpace {
        var loc = myLocales[locIdx];
        if (__primitive("_wide_get_node", loc) != locIdx) then
          halt("In this architecture, we expect the locale whose index is x to live on node x.");
      }
    }

    // Has to be globally unique and not equal to a node ID.
    // We return numLocales for now, since we expect nodes to be numbered less than this.
    // -1 is used in the abstract locale class to specify an invalid node ID.
    proc chpl_id() return numLocales;
    proc chpl_name() return local_name();
    proc local_name() return "rootLocale";

    proc readWriteThis(f) {
      f <~> name;
    }
  
    proc getChildCount() return this.myLocaleSpace.numIndices;

    proc getChildSpace() return this.myLocaleSpace;

    iter getChildIndices() : int {
      for idx in this.myLocaleSpace do
        yield idx;
    }
  
    proc getChild(idx:int) return this.myLocales[idx];
  
    iter getChldren() : locale  {
      for loc in this.myLocales do
        yield loc;
    }

    proc getDefaultLocaleSpace() return this.myLocaleSpace;
    proc getDefaultLocaleArray() return myLocales;

    proc localeIDtoLocale(id : chpl_localeID_t) {
      // In the default architecture, there are only nodes and no sublocales.
      // What is more, the nodeID portion of a wide pointer is the same as the index into
      // myLocales that yields the locale representing that node.
      if __primitive("_loc_get_node", id) == __primitive("chpl_nodeID") then
        return here;
      else
        return myLocales[__primitive("_loc_get_node", id)];
    }
  }
  
  if (rootLocale == nil) then
    rootLocale = new DefaultRootLocale();

  // Expose the underlying locales array (and its domain) 
  // for user convenience and backward compatibility.
  // The downcase is because we cannot move the domain and array return types
  // into the base (abstract) locale class.
  // That would make locales depend on arrays which depend on locales....
  // If we had a way to express domain and array return types abstractly,
  // that problem would go away. <hilde>
  const Locales => (rootLocale:RootLocale).getDefaultLocaleArray();
  const LocaleSpace = Locales.domain;
}
