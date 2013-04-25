(*
    Copyright (c) 2000
        Cambridge University Technical Services Limited

    Modified David C. J. Matthews 2008-2010, 2013

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*)

(* Basic code-tree data structure.  This was previously partly in GCODE.ML
   and partly in CODETREE.ML. *)

structure BaseCodeTree: BaseCodeTreeSig =
struct
    open Address

    datatype argumentType = datatype BackendIntermediateCode.argumentType

    datatype inlineStatus =
        NonInline
    |   MaybeInline
    |   SmallFunction
    |   OnlyInline

    (* How variables are used.  Added and examined by the optimisation pass. *)
    datatype codeUse =
        UseGeneral (* Used in some other context. *)
    |   UseExport  (* Exported i.e. the result of a top-level binding. *)
    |   UseApply of codeUse list * int list
            (* Applied as a function - the list is where the result goes, the int list
               is the width of the tuples for each argument. *)
    |   UseField of int * codeUse list (* Selected as a field - the list is where the result goes *)
    
    datatype codetree =
        Newenv of codeBinding list * codetree (* Set of bindings with an expression. *)

    |   Constnt of machineWord (* Load a constant *)

    |   Extract of loadForm (* Get a local variable, an argument or a closure value *)
    
    |   Indirect of {base: codetree, offset: int, isVariant: bool }
         (* Load a value from a heap record *)
    
    |   Eval of (* Evaluate a function with an argument list. *)
        {
            function:  codetree,
            argList:   (codetree * argumentType) list,
            resultType: argumentType
        }
    
    |   Lambda of lambdaForm (* Lambda expressions. *)

    |   Cond of codetree * codetree * codetree (* If-statement *)
    
    |   BeginLoop of (* Start of tail-recursive inline function. *)
        { loop: codetree, arguments: (simpleBinding * argumentType) list }

    |   Loop of (codetree * argumentType) list (* Jump back to start of tail-recursive function. *)

    |   Raise of codetree (* Raise an exception *)

    |   Ldexc (* Load the exception (used at the start of a handler) *)

    |   Handle of (* Exception handler. *) { exp: codetree, handler: codetree }

    |   Tuple of { fields: codetree list, isVariant: bool } (* Tuples and datatypes *)

    |   Container of int (* Create a container for a tuple on the stack. *)

    |   SetContainer of (* Copy a tuple to a container. *)
        {
            container: codetree,
            tuple:     codetree,
            size:      int
        }

    |   TupleFromContainer of codetree * int (* Make a tuple from the contents of a container. *)

    |   TagTest of { test: codetree, tag: word, maxTag: word }

        (* A constant together with the code for either an inline function or a
           tuple.  This is used for global values. *)
    |   ConstntWithInline of machineWord * envSpecial

    and codeBinding =
        Declar  of simpleBinding (* Make a local declaration or push an argument *)
    |   RecDecs of { addr: int, lambda: lambdaForm, use: codeUse list } list (* Set of mutually recursive declarations. *)
    |   NullBinding of codetree (* Just evaluate the expression and discard the result. *)

    and loadForm =
        LoadArgument of int
    |   LoadLocal of int
    |   LoadClosure of int
    |   LoadRecursive

    (* When we look up an entry in the environment we get a pair of
       a "general" value, which is either a constant or a load, and
       an optional special value, which is either a tuple or an
       inline function.  Tuple entries are functions from an integer
       offset to one of these pairs; inline function entries are a
       lambda together with a map for the free variables. *)
    and envGeneral =
        EnvGenLoad of loadForm | EnvGenConst of machineWord

    and envSpecial =
        EnvSpecNone
    |   EnvSpecTuple of int * (int -> envGeneral * envSpecial)
    |   EnvSpecInlineFunction of lambdaForm * (int -> envGeneral * envSpecial)
    
    withtype simpleBinding = 
    { (* Declare a value or push an argument. *)
        value:      codetree,
        addr:       int,
        use:        codeUse list
    }
    
    and lambdaForm =
    { (* Lambda expressions. *)
        body          : codetree,
        isInline      : inlineStatus,
        name          : string,
        closure       : loadForm list,
        argTypes      : (argumentType * codeUse list) list,
        resultType    : argumentType,
        localCount    : int
    }

    open Pretty

    (* Common cases. *)
    val space = PrettyBreak (1, 0)
    fun block l = PrettyBlock (0, false, [], l)
    val string = PrettyString

    fun pList ([]: 'b list, _: string, _: 'b->pretty) = []
    |   pList ([h],    _, disp) = [disp h]
    |   pList (h::t, sep, disp) =
        PrettyBlock (0, false, [],
            [
                disp h,
                PrettyBreak (0, 0),
                PrettyString sep
            ]
        ) ::
        PrettyBreak (1, 0) ::
        pList (t, sep, disp)

    fun pretty (pt : codetree) : pretty =
    let
        
        fun printList(start, lst, sep) : pretty =
            PrettyBlock (1, true, [],
                PrettyString (start ^ "(") ::
                pList(lst, sep, pretty) @
                [ PrettyBreak (0, 0), PrettyString (")") ]
            )

        fun prettyArg (c, _) = pretty c

        fun prettyArgs(start, lst, sep) : pretty =
            PrettyBlock (1, true, [],
                PrettyString (start ^ "(") ::
                pList(lst, sep, prettyArg) @
                [ PrettyBreak (0, 0), PrettyString (")") ]
            )

    in
        case pt of
            Eval {function, argList, ...} =>
                PrettyBlock (2, false, [],
                    [
                        pretty function,
                        PrettyBreak(1, 2),
                        PrettyBlock(2, true, [],
                            (
                                string "(" ::
                                PrettyBreak(0, 0) ::
                                pList(argList, ",", prettyArg) @
                                [PrettyBreak (0, 0), PrettyString (")")]
                            )
                        )
                    ]
                )
         
        |   Extract(LoadArgument addr) => string ("Arg" ^ Int.toString addr)
        |   Extract(LoadLocal addr) => string ("Local" ^ Int.toString addr)
        |   Extract(LoadClosure addr) => string ("Closure" ^ Int.toString addr)
        |   Extract LoadRecursive => string "Recursive"

        |   Indirect {base, offset, isVariant} =>
                PrettyBlock(2, false, [],
                    [
                        pretty base,
                        PrettyBreak(0, 2),
                        string(concat["[", Int.toString offset, "]", if isVariant then "(*V*)" else ""])
                    ]
                )
        
        | Lambda {body, isInline, name, closure, argTypes, localCount, ...} =>
            let
                val inl = 
                    case isInline of
                      NonInline   => ""
                    | MaybeInline => "inline,"
                    | SmallFunction => "small,"
                    | OnlyInline  => "functor,"
            in
                PrettyBlock(2, true, [],
                [
                    PrettyBlock(4, false, [],
                    [
                        string "fn",
                        space,
                        string "(*", space, string("\"" ^ name ^ "\""), space, string inl,
                        space, string(Int.toString localCount ^ " locals,"), space,
                        string(Int.toString (List.length argTypes) ^ " args,"),
                        space,
                        printList ("closure=", map Extract closure, ","),
                        space, string "*)"
                    ]),
                    PrettyBreak(1, 2),
                    pretty body
                ])
            end
        
        |   Constnt w => PrettyString (stringOfWord w)
        
        |   Cond (f, s, t) =>
            PrettyBlock (0, true, [],
                [
                    PrettyBlock(2, false, [], [string "if", space, pretty f]),
                    space,
                    PrettyBlock(2, false, [], [string "then", space, pretty s]),
                    space,
                    PrettyBlock(2, false, [], [string "else", space, pretty t])
                ]
            )

        | Newenv(decs, final) =>
            PrettyBlock (0, true, [],
                [
                    string "let",
                    PrettyBreak (1, 2),
                    PrettyBlock(2, false, [], pList(decs, ";", prettyBinding)),
                    space,
                    string "in",
                    PrettyBreak(1, 2),
                    PrettyBlock(2, false, [], [pretty final]),
                    space,
                    string "end"
                ]
            )

        | BeginLoop{loop=loopExp, arguments=args } =>
            let
                fun prettyArg (c, _) = prettySimpleBinding c
            in
                PrettyBlock (3, false, [],
                    [
                        PrettyBlock (1, true, [],
                            PrettyString ("BEGINLOOP(") ::
                            pList(args, ",", prettyArg) @
                            [ PrettyBreak (0, 0), PrettyString (")") ]
                        ),
                        PrettyBreak (0, 0),
                        PrettyString "(",
                        PrettyBreak (0, 0),
                        pretty loopExp,
                        PrettyBreak (0, 0),
                        PrettyString ")"
                    ]
                )
            end
        
        |   Loop ptl => prettyArgs("LOOP", ptl, ",")
        
        |   Raise c =>
            PrettyBlock (1, true, [],
                [
                    PrettyString "RAISE(",
                    pretty c,
                    PrettyBreak (0, 0),
                    PrettyString (")")
                ]
            )

        |   Handle {exp, handler, ...} =>
            PrettyBlock (3, false, [],
                [
                    PrettyString "HANDLE(",
                    pretty exp,
                    PrettyString "WITH",
                    PrettyBreak (1, 0),
                    pretty handler,
                    PrettyString ")"
                ]
            )
        
        | Ldexc => PrettyString "LDEXC"
         
        |   Tuple { fields, isVariant } =>
                printList(if isVariant then "DATATYPE" else "TUPLE", fields, ",")
        
        | Container size => PrettyString ("CONTAINER " ^ Int.toString size)
        
        | SetContainer{container, tuple, size} =>
            PrettyBlock (3, false, [],
                [
                    PrettyString ("SETCONTAINER(" ^ Int.toString size ^ ", "),
                    pretty container,
                    PrettyBreak (0, 0),
                    PrettyString ",",
                    PrettyBreak (1, 0),
                    pretty tuple,
                    PrettyBreak (0, 0),
                    PrettyString ")"
                ]
            )
        
        | TupleFromContainer (container, size) =>
            PrettyBlock (3, false, [],
                [
                    PrettyString ("TUPLECONTAINER(" ^ Int.toString size ^ ","),
                    PrettyBreak (0, 0),
                    pretty container,
                    PrettyBreak (0, 0),
                    PrettyString ")"
                ]
            )

        |   TagTest { test, tag, maxTag } =>
            PrettyBlock (3, false, [],
                [
                    PrettyString (concat["TAGTEST(", Word.toString tag, ", ", Word.toString maxTag, ","]),
                    PrettyBreak (1, 0),
                    pretty test,
                    PrettyBreak (0, 0),
                    PrettyString ")"
                ]
            )

        |   ConstntWithInline(w, _) =>
            PrettyBlock (1, true, [],
                [
                    PrettyString "CONSTWITHINLINE (",
                    PrettyString (stringOfWord w),
                    PrettyString ", ",
                    PrettyBreak (1, 0),
                    (*pretty (spec),
                    PrettyBreak (1, 0),*)
                    PrettyString ") (*CONSTWITHINLINE*)"
                ]
            )
        (* That list should be exhaustive! *)
    end (* pretty *)

    and prettyBinding(Declar dec) = prettySimpleBinding dec
       
    |   prettyBinding(RecDecs ptl) =
        let
            fun prettyRDec {lambda, addr, use, ...} =
            block [
                    string ("Local" ^ Int.toString addr),
                    space,
                    string "(*",
                    prettyUses "" use,
                    space,
                    string "*)",
                    space,
                    string "=",
                    PrettyBreak (1, 2),
                    PrettyBlock (2, false, [], [pretty(Lambda lambda)])
                ]
        in
            PrettyBlock(0, true, [],
                string "val rec " ::
                pList(ptl, " and ", prettyRDec)
            )
        end
    |   prettyBinding(NullBinding c) = pretty c

    and prettySimpleBinding{value, addr, use, ...} =
        PrettyBlock (1, false, [],
            [
                string ("val Local" ^ Int.toString addr),
                space,
                string "(*",
                string "",
                space,
                prettyUses "" use,
                space,
                string "*)",
                space,
                string "=",
                PrettyBreak (1, 2),
                PrettyBlock (2, false, [], [pretty value])
            ]
        )

    and prettyUses prefix cl =
           PrettyBlock (1, true, [],
                PrettyString (prefix ^ "[") ::
                pList(cl, ",", prettyUsage) @
                [ PrettyBreak (0, 0), PrettyString ("]") ]
            )

    and prettyUsage UseGeneral = PrettyString "UseGen"
    |   prettyUsage UseExport = PrettyString "UseExp"
    |   prettyUsage (UseApply (cl, al)) =
           PrettyBlock (1, true, [],
                PrettyString "UseApp[" ::
                pList(al, ",", fn i => PrettyString(Int.toString i)) @
                PrettyString "]" ::
                PrettyBreak(1, 0) ::
                PrettyString "[" ::
                pList(cl, ",", prettyUsage) @
                [ PrettyBreak (0, 0), PrettyString "]" ]
            )
    |   prettyUsage (UseField (n, cl)) =
           PrettyBlock (1, true, [],
                PrettyString ("UseField"^ Int.toString n ^ "[") ::
                pList(cl, ",", prettyUsage) @
                [ PrettyBreak (0, 0), PrettyString "]" ]
            )

    (* Mapping function to enable parts of the tree to be replaced. *)
    fun mapCodetree f code =
    let
        (* We use these functions to allow all nodes to be processed even if
           they are not full codetree nodes. *)
        fun deExtract(Extract l) = l | deExtract _ = raise Misc.InternalError "deExtract"
        fun deLambda (Lambda l) = l | deLambda _ = raise Misc.InternalError "deLambda"

        fun mapt (Newenv(decs, exp)) =
            let
                fun mapbinding(Declar{value, addr, use}) = Declar{value=mapCodetree f value, addr=addr, use=use}
                |   mapbinding(RecDecs l) =
                        RecDecs(map(fn {addr, lambda, use} =>
                            {addr=addr, use = use, lambda = deLambda(mapCodetree f (Lambda lambda))}) l)
                |   mapbinding(NullBinding exp) = NullBinding(mapCodetree f exp)
            in
                Newenv(map mapbinding decs, mapCodetree f exp)
            end
        |   mapt (c as Constnt _) = c
        |   mapt (e as Extract _) = e
        |   mapt (Indirect { base, offset, isVariant }) =
                Indirect{ base = mapCodetree f base, offset = offset, isVariant = isVariant }
        |   mapt (Eval { function, argList, resultType }) =
                Eval {
                    function = mapCodetree f function, 
                    argList = map (fn(c, a) => (mapCodetree f c, a)) argList,
                    resultType = resultType
                }
        |   mapt (Lambda { body, isInline, name, closure, argTypes, resultType, localCount }) =
                Lambda {
                    body = mapCodetree f body, isInline = isInline, name = name,
                    closure = map (deExtract o (mapCodetree f) o Extract) closure,
                    argTypes = argTypes, resultType = resultType, localCount = localCount
                }
        |   mapt (Cond(i, t, e)) = Cond(mapCodetree f i, mapCodetree f t, mapCodetree f e)
        |   mapt (BeginLoop{loop, arguments}) =
                BeginLoop {
                    loop = mapCodetree f loop,
                    arguments = map(fn({value, addr, use}, t) => ({value=mapCodetree f value, addr=addr, use=use}, t)) arguments
                
                }
        |   mapt (Loop l) = Loop (map(fn(c, t) => (mapCodetree f c, t)) l)
        |   mapt (Raise r) = Raise(mapCodetree f r)
        |   mapt Ldexc = Ldexc
        |   mapt (Handle{exp, handler}) = Handle{exp=mapCodetree f exp, handler=mapCodetree f handler }
        |   mapt (Tuple { fields, isVariant} ) = Tuple { fields = map (mapCodetree f) fields, isVariant = isVariant }
        |   mapt (c as Container _) = c
        |   mapt (SetContainer{container, tuple, size}) =
                SetContainer{
                    container = mapCodetree f container, tuple = mapCodetree f tuple, size = size }
        |   mapt (TupleFromContainer(c, s)) = TupleFromContainer(mapCodetree f c, s)
        |   mapt (TagTest{test, tag, maxTag}) = TagTest{test = mapCodetree f test, tag = tag, maxTag = maxTag }
        |   mapt (c as ConstntWithInline _) = c
    in
        (* Apply f to node.  If it returns SOME c use that otherwise
           traverse the tree. *)
        case f code of
            SOME c => c
        |   NONE => mapt code
    end

    (* Fold a function over the tree.  f is applied to the node and the input
       value and returns an output and a flag.  If the flag is
       FOLD_DONT_DESCEND the output value is used and the code tree is not
       examined further.  Otherwise this function descends into the tree and
       folds over the subtree. *)
    datatype foldControl = FOLD_DESCEND | FOLD_DONT_DESCEND

    fun foldtree (f: codetree * 'a -> 'a * foldControl) (input: 'a) code =
    let
        fun ftree (Newenv(decs, exp), v) =
            let
                fun foldbinding(Declar{value, ...}, w) = foldtree f w value
                |   foldbinding(RecDecs l, w) =
                        foldl(fn ({lambda, ...}, x) => foldtree f x (Lambda lambda)) w l
                |   foldbinding(NullBinding exp, w) = foldtree f w exp
            in
                foldtree f (foldl foldbinding v decs) exp
            end
        |   ftree (Constnt _, v) = v
        |   ftree (Extract _, v) = v
        |   ftree (Indirect{base, ...}, v) = foldtree f v base
        |   ftree (Eval { function, argList, ...}, v) =
                foldl(fn((c, _), w) => foldtree f w c) (foldtree f v function) argList
        |   ftree (Lambda { body, closure, ...}, v) =
                foldtree f (foldl (fn (c, w) => foldtree f w (Extract c)) v closure) body
        |   ftree (Cond(i, t, e), v) = foldtree f (foldtree f (foldtree f v i) t) e
        |   ftree (BeginLoop{loop, arguments, ...}, v) =
                foldtree f (foldl (fn (({value, ...}, _), w) => foldtree f w value) v arguments) loop
        |   ftree (Loop l, v) = foldl (fn ((c, _), w) => foldtree f w c) v l
        |   ftree (Raise r, v) = foldtree f v r
        |   ftree (Ldexc, v) = v
        |   ftree (Handle{exp, handler}, v) = foldtree f (foldtree f v exp) handler
        |   ftree (Tuple { fields, ...}, v) = foldl (fn (c, w) => foldtree f w c) v fields
        |   ftree (Container _, v) = v
        |   ftree (SetContainer { container, tuple, ...}, v) = foldtree f (foldtree f v container) tuple
        |   ftree (TupleFromContainer(c, _), v) = foldtree f v c
        |   ftree (TagTest{test, ...}, v) = foldtree f v test
        |   ftree (ConstntWithInline _, v) = v
    in
        case f (code, input) of
            (v, FOLD_DONT_DESCEND) => v
        |   (v, FOLD_DESCEND) => ftree(code, v)
    end

    structure Sharing =
    struct
        type codetree = codetree
        and  pretty = pretty
        and  inlineStatus = inlineStatus
        and  argumentType = argumentType
        and  codeBinding = codeBinding
        and  simpleBinding = simpleBinding
        and  loadForm = loadForm
        and  envGeneral = envGeneral
        and  envSpecial = envSpecial
        and  codeUse = codeUse
        and  foldControl = foldControl
    end

end;
