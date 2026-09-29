// Weapon no-recoil patch:
//   - public static bool NoRecoil (default false) + SetNoRecoil(bool) ekler
//   - Awake() sonuna: if (NoRecoil) { _recoilKnockback = 0; _spread = 0f; }
//   - AddModelRecoil(float) basina: if (NoRecoil) return;
// Kullanim: RecoilPatch.exe <in.dll> <out.dll> [managedDir]
// (managedDir: FishNet.Runtime cozumleme icin; verilmezse in.dll dizini + oyun klasoru denenir)
using Mono.Cecil;
using Mono.Cecil.Cil;

static class Patch
{
    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.WriteLine("Kullanim: RecoilPatch.exe <in.dll> <out.dll> [managedDir]");
            return 1;
        }
        var dll = args[0];
        var outDll = args[1];
        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(Path.GetDirectoryName(dll));
        if (args.Length > 2) resolver.AddSearchDirectory(args[2]);
        resolver.AddSearchDirectory(
            @"C:\Users\emirs\Downloads\How.to.Fish.v1.0.10-OFME\How to Fish\How to Fish_Data\Managed");
        var asm = AssemblyDefinition.ReadAssembly(dll,
            new ReaderParameters { AssemblyResolver = resolver });
        var mod = asm.MainModule;
        var wpn = mod.Types.FirstOrDefault(t => t.Name == "Weapon");
        if (wpn == null) { Console.WriteLine("[!] Weapon yok."); return 2; }
        if (wpn.Fields.Any(f => f.Name == "NoRecoil"))
        {
            Console.WriteLine("[!] Zaten patch'li (NoRecoil var). Cikiliyor.");
            return 3;
        }

        var fKnock = wpn.Fields.FirstOrDefault(f => f.Name == "_recoilKnockback");
        var fSpread = wpn.Fields.FirstOrDefault(f => f.Name == "_spread");
        if (fKnock == null || fSpread == null)
        {
            Console.WriteLine("[!] _recoilKnockback/_spread bulunamadi.");
            return 4;
        }
        if (fKnock.FieldType.FullName != "System.Int32" ||
            fSpread.FieldType.FullName != "System.Single")
        {
            Console.WriteLine($"[!] tip farki: knock={fKnock.FieldType} spread={fSpread.FieldType}");
            return 5;
        }

        // --- 1) static bool NoRecoil + SetNoRecoil(bool)
        var flag = new FieldDefinition("NoRecoil",
            FieldAttributes.Public | FieldAttributes.Static, mod.TypeSystem.Boolean);
        wpn.Fields.Add(flag);
        var set = new MethodDefinition("SetNoRecoil",
            MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            mod.TypeSystem.Void);
        set.Parameters.Add(new ParameterDefinition("value", ParameterAttributes.None, mod.TypeSystem.Boolean));
        var sil = set.Body.GetILProcessor();
        sil.Emit(OpCodes.Ldarg_0);
        sil.Emit(OpCodes.Stsfld, flag);
        sil.Emit(OpCodes.Ret);
        wpn.Methods.Add(set);
        Console.WriteLine("[+] NoRecoil + SetNoRecoil eklendi.");

        // --- 2) Awake() sonu: if (NoRecoil) { knock=0; spread=0f; }
        var awake = wpn.Methods.FirstOrDefault(m => m.Name == "Awake" && !m.HasParameters);
        if (awake == null || !awake.HasBody) { Console.WriteLine("[!] Awake yok."); return 6; }
        var ail = awake.Body.GetILProcessor();
        var aret = awake.Body.Instructions.LastOrDefault(i => i.OpCode == OpCodes.Ret);
        if (aret == null) { Console.WriteLine("[!] Awake ret yok."); return 7; }
        foreach (var s in new[]
        {
            ail.Create(OpCodes.Ldsfld, flag),
            ail.Create(OpCodes.Brfalse_S, aret),
            ail.Create(OpCodes.Ldarg_0),
            ail.Create(OpCodes.Ldc_I4_0),
            ail.Create(OpCodes.Stfld, fKnock),
            ail.Create(OpCodes.Ldarg_0),
            ail.Create(OpCodes.Ldc_R4, 0f),
            ail.Create(OpCodes.Stfld, fSpread),
        }) ail.InsertBefore(aret, s);
        Console.WriteLine("[+] Awake sifirlama gomuldu.");

        // --- 3) AddModelRecoil(float) basi: if (NoRecoil) return;
        var amr = wpn.Methods.FirstOrDefault(m => m.Name == "AddModelRecoil" &&
            m.Parameters.Count == 1 &&
            m.Parameters[0].ParameterType.FullName == "System.Single");
        if (amr == null || !amr.HasBody) { Console.WriteLine("[!] AddModelRecoil(float) yok."); return 8; }
        var mil = amr.Body.GetILProcessor();
        var first = amr.Body.Instructions[0];
        mil.InsertBefore(first, mil.Create(OpCodes.Ldsfld, flag));
        mil.InsertBefore(first, mil.Create(OpCodes.Brfalse_S, first));
        mil.InsertBefore(first, mil.Create(OpCodes.Ret));
        Console.WriteLine("[+] AddModelRecoil guard gomuldu.");

        asm.Write(outDll);
        Console.WriteLine("[+] Yazildi: " + outDll);
        return 0;
    }
}
