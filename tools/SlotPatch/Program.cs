// SlotMachineManager patch.
// Modlar:
//   yellow (default): SARI GARANTI TESTI - RollRandom() icine SendRoll oncesi
//                     direkt "rolled = (byte)num3;" gomer. Enum/menu yok.
//   switch:           CasinoForceResult enum + ForcedResult field +
//                     SetForcedResult metodu + Red->num / Yellow->num3 switch'i.
// Kullanim: SlotPatch.exe <Assembly-CSharp.dll> [cikti.dll] [yellow|switch]
using Mono.Cecil;
using Mono.Cecil.Cil;

static class Patch
{
    static FieldDefinition AddLiteral(TypeDefinition en, string name, int value)
    {
        var f = new FieldDefinition(name,
            FieldAttributes.Public | FieldAttributes.Static |
            FieldAttributes.Literal | FieldAttributes.HasDefault,
            en);
        f.Constant = value;
        en.Fields.Add(f);
        return f;
    }

    static bool IsVarLd(Instruction ins, int idx) =>
        (ins.OpCode == OpCodes.Ldloc_S || ins.OpCode == OpCodes.Ldloc) &&
        ins.Operand is VariableDefinition vd && vd.Index == idx;

    static bool IsVarSt(Instruction ins, int idx) =>
        (ins.OpCode == OpCodes.Stloc_S || ins.OpCode == OpCodes.Stloc) &&
        ins.Operand is VariableDefinition vd && vd.Index == idx;

    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.WriteLine("Kullanim: SlotPatch.exe <Assembly-CSharp.dll> [cikti.dll] [yellow|switch]");
            return 1;
        }
        var dll = args[0];
        var outDll = args.Length > 1 ? args[1] : dll;
        var mode = args.Length > 2 ? args[2].ToLowerInvariant() : "yellow";
        if (mode != "yellow" && mode != "switch")
        {
            Console.WriteLine("[!] mod yellow|switch olmali.");
            return 1;
        }
        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(Path.GetDirectoryName(dll));
        var asm = AssemblyDefinition.ReadAssembly(dll,
            new ReaderParameters { AssemblyResolver = resolver });
        var mod = asm.MainModule;
        var smm = mod.Types.FirstOrDefault(t => t.Name == "SlotMachineManager");
        if (smm == null) { Console.WriteLine("[!] SlotMachineManager yok."); return 2; }

        var rr = smm.Methods.FirstOrDefault(m => m.Name == "RollRandom");
        if (rr == null || !rr.HasBody) { Console.WriteLine("[!] RollRandom yok/body yok."); return 4; }
        var vars = rr.Body.Variables;
        if (vars.Count <= 11)
        {
            Console.WriteLine($"[!] local sayisi yetersiz: {vars.Count}");
            return 5;
        }
        // V_6=rolled(Byte), V_4/V_5=itemIDs/itemSkins(Byte[]), V_7=num(Int32), V_11=num3(Int32)
        string TN(int i) => vars[i].VariableType.FullName;
        if (TN(6) != "System.Byte" || TN(4) != "System.Byte[]" ||
            TN(5) != "System.Byte[]" || TN(7) != "System.Int32" || TN(11) != "System.Int32")
        {
            Console.WriteLine("[!] local tipleri beklenenden farkli:");
            for (int i = 0; i < Math.Min(vars.Count, 18); i++)
                Console.WriteLine($"    V_{i} = {TN(i)}");
            return 6;
        }
        var vRolled = vars[6];
        var vNum = vars[7];
        var vNum3 = vars[11];

        // Anchor: SendRoll callvirt'ten onceki ldsfld _instance
        var insns = rr.Body.Instructions;
        Instruction anchor = null;
        for (int i = 0; i < insns.Count; i++)
        {
            var ins = insns[i];
            if (ins.OpCode == OpCodes.Callvirt &&
                ins.Operand is MethodReference mr && mr.Name == "SendRoll")
            {
                // geriye: ldloc V_6, ldloc V_5, ldloc V_4, ldarg.0, ldsfld _instance
                if (i >= 5 && insns[i - 5].OpCode == OpCodes.Ldsfld &&
                    insns[i - 5].Operand is FieldReference fr && fr.Name == "_instance")
                    anchor = insns[i - 5];
                break;
            }
        }
        if (anchor == null) { Console.WriteLine("[!] SendRoll cagrisi bulunamadi."); return 7; }

        var il = rr.Body.GetILProcessor();

        if (mode == "yellow")
        {
            // Idempotency: anchor oncesi ldloc.s V_11 / conv.u1 / stloc.s V_6 varsa atla
            int ai = insns.IndexOf(anchor);
            if (ai >= 3 && IsVarLd(insns[ai - 3], 11) &&
                insns[ai - 2].OpCode == OpCodes.Conv_U1 && IsVarSt(insns[ai - 1], 6))
            {
                Console.WriteLine("[!] Zaten sari-test patch'li. Cikiliyor.");
                return 3;
            }
            il.InsertBefore(anchor, il.Create(OpCodes.Ldloc_S, vNum3));
            il.InsertBefore(anchor, il.Create(OpCodes.Conv_U1));
            il.InsertBefore(anchor, il.Create(OpCodes.Stloc_S, vRolled));
            Console.WriteLine("[+] RollRandom SARI TEST gomuldu: rolled = (byte)num3.");
        }
        else
        {
            if (smm.Fields.Any(f => f.Name == "ForcedResult"))
            {
                Console.WriteLine("[!] Zaten patch'li (ForcedResult var). Cikiliyor.");
                return 3;
            }
            // --- 1) nested enum CasinoForceResult : int { Normal=0, Red=1, Yellow=2 }
            var enumType = new TypeDefinition("", "CasinoForceResult",
                TypeAttributes.NestedPublic | TypeAttributes.Sealed | TypeAttributes.AnsiClass,
                mod.ImportReference(typeof(Enum)));
            smm.NestedTypes.Add(enumType);
            enumType.Fields.Add(new FieldDefinition("value__",
                FieldAttributes.Public | FieldAttributes.SpecialName | FieldAttributes.RTSpecialName,
                mod.TypeSystem.Int32));
            AddLiteral(enumType, "Normal", 0);
            AddLiteral(enumType, "Red", 1);
            AddLiteral(enumType, "Yellow", 2);
            Console.WriteLine("[+] enum CasinoForceResult eklendi.");

            // --- 2) public static CasinoForceResult ForcedResult (default 0 = Normal)
            var forced = new FieldDefinition("ForcedResult",
                FieldAttributes.Public | FieldAttributes.Static, enumType);
            smm.Fields.Add(forced);
            Console.WriteLine("[+] static ForcedResult eklendi.");

            // --- 3) public static void SetForcedResult(CasinoForceResult result)
            var set = new MethodDefinition("SetForcedResult",
                MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
                mod.TypeSystem.Void);
            set.Parameters.Add(new ParameterDefinition("result", ParameterAttributes.None, enumType));
            var sil = set.Body.GetILProcessor();
            sil.Emit(OpCodes.Ldarg_0);
            sil.Emit(OpCodes.Stsfld, forced);
            sil.Emit(OpCodes.Ret);
            smm.Methods.Add(set);
            Console.WriteLine("[+] SetForcedResult eklendi.");

            // --- 4) switch
            var iRed = il.Create(OpCodes.Ldloc_S, vNum);
            var iYel = il.Create(OpCodes.Ldloc_S, vNum3);
            var iEnd = anchor;
            var seq = new List<Instruction>
            {
                il.Create(OpCodes.Ldsfld, forced),
                il.Create(OpCodes.Switch, new[] { iRed, iYel }),
                il.Create(OpCodes.Br_S, iEnd),
                iRed,
                il.Create(OpCodes.Conv_U1),
                il.Create(OpCodes.Stloc_S, vRolled),
                il.Create(OpCodes.Br_S, iEnd),
                iYel,
                il.Create(OpCodes.Conv_U1),
                il.Create(OpCodes.Stloc_S, vRolled),
            };
            foreach (var s in seq) il.InsertBefore(anchor, s);
            Console.WriteLine("[+] RollRandom switch gomuldu (Red->num, Yellow->num3).");
        }

        asm.Write(outDll);
        Console.WriteLine("[+] Yazildi: " + outDll);
        return 0;
    }
}
