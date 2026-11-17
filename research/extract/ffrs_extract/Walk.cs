// Begehbarer Boden aus der Kollision der Spielwelt (Wunsch User 16.11.: Karten ohne vollstaendiges Navmesh).
// walk <ausgabe> <regex der Level>: sammelt die Kollision aller passenden Level (Kollisionsformen der Modelle, bei
// "komplex als einfach" die Dreiecke), rastert sie wie Recast in Saeulen, prueft Stehhoehe, Stufen und Abstand zur
// Wand und schreibt das 75-cm-Raster im Format der Mod (ohne Aufzuege und Sperren, die ergaenzt build_kollkarten.py).
using System.Globalization;
using System.Text.RegularExpressions;
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Component.StaticMesh;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.PhysicsEngine;
using CUE4Parse.UE4.Objects.UObject;

static class Walk
{
    // Figur (Mod-Log: Mitte der Kapsel 93 cm ueber dem Boden), Stufe und Steigung wie bei UE-Figuren ueblich.
    const double Cs = 15, Ch = 5, AgentHeight = 180, AgentRadius = 34, Climb = 45, Grid = 75;
    static readonly double WalkableNormalZ = Math.Cos(46 * Math.PI / 180);

    struct Span { public int Min, Max; public bool Walk; }

    static readonly Dictionary<string, List<double[]>?> MeshCache = new();
    static readonly Dictionary<string, int> Stats = new();
    static int Diagnosed;
    static void Count(string key) => Stats[key] = Stats.GetValueOrDefault(key) + 1;

    public static void Run(DefaultFileProvider provider, string[] args, string outDir)
    {
        PropertyUtil.SearchPropertyInTemplate = true;
        var regex = new Regex(args[2], RegexOptions.IgnoreCase);
        var tris = new List<double[]>();
        foreach (var key in provider.Files.Keys.Where(k => k.EndsWith(".umap") && regex.IsMatch(k)
                     && !Regex.IsMatch(k, "/(NAV|LT|SD)/")).OrderBy(k => k))
        {
            var before = tris.Count;
            try
            {
                foreach (var export in provider.LoadPackage(key).GetExports())
                    if (export is UStaticMeshComponent component)
                        AddComponent(component, tris);
            }
            catch (Exception error)
            {
                Console.WriteLine($"{key}: Fehler {error.Message}");
            }
            Console.WriteLine($"{key}: {tris.Count - before} Dreiecke");
        }
        foreach (var pair in Stats.OrderBy(p => p.Key))
            Console.WriteLine($"  {pair.Value,6} {pair.Key}");
        Build(tris, Path.Combine(outDir, args[1]));
    }

    // ---------- Kollision sammeln ----------

    static FTransform Relative(UObject c) => new(
        c.GetOrDefault("RelativeRotation", FRotator.ZeroRotator).Quaternion(),
        c.GetOrDefault("RelativeLocation", FVector.ZeroVector),
        c.GetOrDefault("RelativeScale3D", FVector.OneVector));

    static List<FTransform> Chain(UObject component)
    {
        var chain = new List<FTransform>();
        UObject? current = component;
        for (var guard = 0; current != null && guard < 16; guard++)
        {
            chain.Add(Relative(current));
            var parent = current.GetOrDefault<FPackageIndex?>("AttachParent");
            current = parent == null || parent.IsNull ? null : parent.Load<UObject>();
        }
        return chain;
    }

    static string OwnerClass(UObject component)
    {
        var outer = component.Outer?.Object?.Value;
        return outer?.ExportType ?? "";
    }

    static void AddComponent(UStaticMeshComponent component, List<double[]> tris)
    {
        var owner = OwnerClass(component);
        // Figuren, Ausloeser und bewegliche Objekte (Aufzuege, Tueren, Sperren) nicht: Die prueft die Mod live.
        if (owner.StartsWith("BP_MapUnit") || owner.StartsWith("BP_MapGimmick") || owner.StartsWith("BP_Map")
            || owner.Contains("Elevator") || owner.Contains("Door"))
        {
            Count("Besitzer ausgelassen " + owner);
            return;
        }
        var body = component.GetOrDefault<FStructFallback?>("BodyInstance");
        var profile = body?.GetOrDefault<FName>("CollisionProfileName").Text ?? "";
        var enabled = body?.GetOrDefault<FName>("CollisionEnabled").Text ?? "";
        if (profile is "" or "None" or "Default")
            profile = "";
        var meshIndex = component.GetOrDefault<FPackageIndex?>("StaticMesh");
        if (meshIndex == null || meshIndex.IsNull)
        {
            Count("ohne Modell");
            return;
        }
        var local = MeshTris(meshIndex);
        if (local == null || local.Count == 0)
        {
            Count("Modell ohne Geometrie");
            return;
        }
        if (profile == "")
        {
            var meshDefault = MeshProfiles.GetValueOrDefault(meshIndex.ResolvedObject?.GetPathName() ?? meshIndex.Name, "/");
            var cut = meshDefault.IndexOf('/');
            profile = meshDefault[..cut];
            if (enabled == "" || enabled.Contains("None"))
                enabled = meshDefault[(cut + 1)..];
        }
        if (!Blocks(profile, enabled))
        {
            Count("Profil ausgelassen " + profile + "/" + enabled);
            return;
        }
        Count("Profil " + (profile == "" ? "(Standard)" : profile) + " " + owner);
        var chain = Chain(component);
        if (component is UInstancedStaticMeshComponent instanced && instanced.PerInstanceSMData != null)
        {
            foreach (var instance in instanced.PerInstanceSMData)
                AddTransformed(local, instance.TransformData, chain, tris);
            return;
        }
        AddTransformed(local, null, chain, tris);
    }

    static void AddTransformed(List<double[]> local, FTransform? first, List<FTransform> chain, List<double[]> tris)
    {
        foreach (var t in local)
        {
            var world = new double[9];
            for (var k = 0; k < 3; k++)
            {
                var v = new FVector((float)t[k * 3], (float)t[k * 3 + 1], (float)t[k * 3 + 2]);
                if (first != null)
                    v = first.Value.TransformPosition(v);
                foreach (var step in chain)
                    v = step.TransformPosition(v);
                world[k * 3] = v.X;
                world[k * 3 + 1] = v.Y;
                world[k * 3 + 2] = v.Z;
            }
            tris.Add(world);
        }
    }

    static List<double[]>? MeshTris(FPackageIndex meshIndex)
    {
        var key = meshIndex.ResolvedObject?.GetPathName() ?? meshIndex.Name;
        if (MeshCache.TryGetValue(key, out var cached))
            return cached;
        List<double[]>? result = null;
        LastProfile = "/";
        try
        {
            var mesh = meshIndex.Load<UStaticMesh>();
            if (mesh != null)
                result = MeshCollision(mesh);
        }
        catch (Exception error)
        {
            Console.WriteLine($"Modell {key}: {error.Message}");
        }
        MeshCache[key] = result;
        MeshProfiles[key] = LastProfile;
        return result;
    }

    // Die einfachen Kollisionsformen sind beim Kochen entfernt worden (AggGeom leer, nur Chaos-Binaerdaten, die CUE4Parse
    // nicht liest). Ersatz: die sichtbare Geometrie aller Modelle, deren Profil die Figur blockiert.
    static readonly Dictionary<string, string> MeshProfiles = new();
    static string LastProfile = "/";

    static List<double[]>? MeshCollision(UStaticMesh mesh)
    {
        var body = mesh.BodySetup.IsNull ? null : mesh.BodySetup.Load<UBodySetup>();
        if (body == null)
        {
            Count("Modell ohne BodySetup");
            return null;
        }
        var defaults = body.GetOrDefault<FStructFallback?>("DefaultInstance");
        var meshProfile = defaults?.GetOrDefault<FName>("CollisionProfileName").Text ?? "";
        var meshEnabled = defaults?.GetOrDefault<FName>("CollisionEnabled").Text ?? "";
        LastProfile = meshProfile + "/" + meshEnabled;
        var tris = new List<double[]>();
        var lod = mesh.RenderData?.LODs?.FirstOrDefault(l => !l.SkipLod);
        if (lod?.PositionVertexBuffer == null || lod.IndexBuffer?.Buffer == null)
            return null;
        var verts = lod.PositionVertexBuffer.Verts;
        var idx = lod.IndexBuffer.Buffer;
        for (var i = 0; i + 2 < idx.Length; i += 3)
        {
            var a = verts[idx[i]];
            var b = verts[idx[i + 1]];
            var c = verts[idx[i + 2]];
            tris.Add(new double[] { a.X, a.Y, a.Z, b.X, b.Y, b.Z, c.X, c.Y, c.Z });
        }
        return tris;
    }

    static bool Blocks(string profile, string enabled) =>
        !(profile is "NoCollision" or "OverlapAll" or "OverlapAllDynamic" or "Trigger" or "UI" or "Spectator" or "IgnoreOnlyPawn"
          || enabled.Contains("NoCollision") || enabled.Contains("PhysicsOnly") || enabled.Contains("ProbeOnly"));

    static void AddBox(List<double[]> tris, FVector center, FRotator rotation, double hx, double hy, double hz)
    {
        var transform = new FTransform(rotation, center, FVector.OneVector);
        var corners = new FVector[8];
        for (var i = 0; i < 8; i++)
            corners[i] = transform.TransformPosition(new FVector((float)((i & 1) == 0 ? -hx : hx),
                (float)((i & 2) == 0 ? -hy : hy), (float)((i & 4) == 0 ? -hz : hz)));
        int[][] faces = { new[] { 0, 1, 3, 2 }, new[] { 4, 6, 7, 5 }, new[] { 0, 4, 5, 1 }, new[] { 2, 3, 7, 6 },
            new[] { 0, 2, 6, 4 }, new[] { 1, 5, 7, 3 } };
        foreach (var f in faces)
        {
            AddTri(tris, corners[f[0]], corners[f[1]], corners[f[2]]);
            AddTri(tris, corners[f[0]], corners[f[2]], corners[f[3]]);
        }
    }

    static void AddTri(List<double[]> tris, FVector a, FVector b, FVector c) =>
        tris.Add(new double[] { a.X, a.Y, a.Z, b.X, b.Y, b.Z, c.X, c.Y, c.Z });

    // Konvexe Huelle durch Ausprobieren aller Dreiecke (die Koerper haben selten mehr als 60 Punkte).
    static void AddConvex(List<double[]> tris, FKConvexElem convex)
    {
        var points = convex.VertexData.Select(v => convex.Transform.TransformPosition(v)).ToArray();
        var idx = convex.IndexData;
        if (idx.Length >= 3)
        {
            for (var i = 0; i + 2 < idx.Length; i += 3)
                AddTri(tris, points[idx[i]], points[idx[i + 1]], points[idx[i + 2]]);
            return;
        }
        var n = Math.Min(points.Length, 120);
        if (n < 4)
            return;
        for (var i = 0; i < n; i++)
        for (var j = i + 1; j < n; j++)
        for (var k = j + 1; k < n; k++)
        {
            var a = points[i];
            var ab = points[j] - a;
            var ac = points[k] - a;
            var normal = new FVector(ab.Y * ac.Z - ab.Z * ac.Y, ab.Z * ac.X - ab.X * ac.Z, ab.X * ac.Y - ab.Y * ac.X);
            var length = Math.Sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z);
            if (length < 1e-3)
                continue;
            bool above = false, below = false;
            for (var m = 0; m < n && !(above && below); m++)
            {
                var d = points[m] - a;
                var side = (normal.X * d.X + normal.Y * d.Y + normal.Z * d.Z) / length;
                if (side > 0.5) above = true;
                else if (side < -0.5) below = true;
            }
            if (!(above && below))
                AddTri(tris, a, points[j], points[k]);
        }
    }

    // ---------- Rastern (wie Recast: rcRasterizeTriangles, Filter, Erosion) ----------

    static double ox, oy, oz;
    static int width, height;
    static List<Span>?[] columns = Array.Empty<List<Span>?>();

    static void Build(List<double[]> tris, string target)
    {
        if (tris.Count == 0)
        {
            Console.WriteLine("Keine Kollision gefunden.");
            return;
        }
        double minX = double.MaxValue, minY = double.MaxValue, minZ = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var t in tris)
            for (var k = 0; k < 3; k++)
            {
                minX = Math.Min(minX, t[k * 3]); maxX = Math.Max(maxX, t[k * 3]);
                minY = Math.Min(minY, t[k * 3 + 1]); maxY = Math.Max(maxY, t[k * 3 + 1]);
                minZ = Math.Min(minZ, t[k * 3 + 2]);
            }
        ox = minX - Cs; oy = minY - Cs; oz = minZ - Ch;
        width = (int)Math.Ceiling((maxX - ox) / Cs) + 2;
        height = (int)Math.Ceiling((maxY - oy) / Cs) + 2;
        Console.WriteLine($"Dreiecke {tris.Count}, Feld {width}x{height} ({(maxX - minX) / 100:F0} x {(maxY - minY) / 100:F0} m)");
        columns = new List<Span>?[width * height];
        foreach (var t in tris)
            RasterTri(t);
        Filter();
        var open = BuildOpen();
        Erode(open);
        WriteGrid(open, target);
    }

    static void RasterTri(double[] t)
    {
        var ax = t[3] - t[0]; var ay = t[4] - t[1]; var az = t[5] - t[2];
        var bx = t[6] - t[0]; var by = t[7] - t[1]; var bz = t[8] - t[2];
        var nx = ay * bz - az * by; var ny = az * bx - ax * bz; var nz = ax * by - ay * bx;
        var len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9)
            return;
        // Beide Seiten zaehlen: Bei gespiegelten Modellen zeigt die Normale nach unten.
        var walkable = Math.Abs(nz / len) >= WalkableNormalZ;
        var poly = new List<double[]> { new[] { t[0], t[1], t[2] }, new[] { t[3], t[4], t[5] }, new[] { t[6], t[7], t[8] } };
        var y0 = (int)Math.Floor((Math.Min(t[1], Math.Min(t[4], t[7])) - oy) / Cs);
        var y1 = (int)Math.Floor((Math.Max(t[1], Math.Max(t[4], t[7])) - oy) / Cs);
        y0 = Math.Clamp(y0, 0, height - 1);
        y1 = Math.Clamp(y1, 0, height - 1);
        var rest = poly;
        for (var y = y0; y <= y1; y++)
        {
            Divide(rest, oy + (y + 1) * Cs, 1, out var row, out rest);
            if (row.Count < 3)
                continue;
            var minX = row.Min(v => v[0]);
            var maxX = row.Max(v => v[0]);
            var x0 = Math.Clamp((int)Math.Floor((minX - ox) / Cs), 0, width - 1);
            var x1 = Math.Clamp((int)Math.Floor((maxX - ox) / Cs), 0, width - 1);
            var rowRest = row;
            for (var x = x0; x <= x1; x++)
            {
                Divide(rowRest, ox + (x + 1) * Cs, 0, out var cell, out rowRest);
                if (cell.Count < 3)
                    continue;
                var smin = cell.Min(v => v[2]);
                var smax = cell.Max(v => v[2]);
                AddSpan(x, y, (int)Math.Floor((smin - oz) / Ch), Math.Max((int)Math.Ceiling((smax - oz) / Ch), (int)Math.Floor((smin - oz) / Ch) + 1), walkable);
            }
        }
    }

    static void Divide(List<double[]> input, double line, int axis, out List<double[]> below, out List<double[]> above)
    {
        below = new List<double[]>();
        above = new List<double[]>();
        var n = input.Count;
        for (int i = 0, j = n - 1; i < n; j = i, i++)
        {
            var di = line - input[i][axis];
            var dj = line - input[j][axis];
            var ina = dj >= 0;
            var inb = di >= 0;
            if (ina != inb)
            {
                var s = dj / (dj - di);
                var p = new[] { input[j][0] + (input[i][0] - input[j][0]) * s, input[j][1] + (input[i][1] - input[j][1]) * s,
                    input[j][2] + (input[i][2] - input[j][2]) * s };
                below.Add(p);
                above.Add(p);
                if (di > 0) below.Add(input[i]);
                else if (di < 0) above.Add(input[i]);
            }
            else
            {
                if (di >= 0)
                {
                    below.Add(input[i]);
                    if (di != 0) continue;
                }
                above.Add(input[i]);
            }
        }
    }

    static void AddSpan(int x, int y, int smin, int smax, bool walk)
    {
        var list = columns[x + y * width] ??= new List<Span>(2);
        var span = new Span { Min = smin, Max = smax, Walk = walk };
        var i = 0;
        while (i < list.Count)
        {
            var cur = list[i];
            if (cur.Min > span.Max)
                break;
            if (cur.Max < span.Min)
            {
                i++;
                continue;
            }
            if (Math.Abs(span.Max - cur.Max) <= 1)
                span.Walk |= cur.Walk;
            else if (cur.Max > span.Max)
                span.Walk = cur.Walk;
            span.Min = Math.Min(span.Min, cur.Min);
            span.Max = Math.Max(span.Max, cur.Max);
            list.RemoveAt(i);
        }
        list.Insert(i, span);
    }

    static void Filter()
    {
        var climb = (int)(Climb / Ch);
        var need = (int)Math.Ceiling(AgentHeight / Ch);
        foreach (var list in columns)
        {
            if (list == null) continue;
            // Niedrige Kanten (Bordstein, Stufe) auf begehbarem Grund sind selbst begehbar.
            var previousWalk = false;
            var previousMax = int.MinValue;
            for (var i = 0; i < list.Count; i++)
            {
                var s = list[i];
                var walk = s.Walk;
                if (!s.Walk && previousWalk && s.Max - previousMax <= climb)
                    s.Walk = true;
                previousWalk = walk;
                previousMax = s.Max;
                // Zu wenig Platz bis zur Decke.
                var ceiling = i + 1 < list.Count ? list[i + 1].Min : int.MaxValue;
                if ((long)ceiling - s.Max < need)
                    s.Walk = false;
                list[i] = s;
            }
        }
    }

    // Begehbare Oberflaechen mit Nachbarn in vier Richtungen.
    sealed class OpenSet
    {
        public readonly List<int> Column = new(), Floor = new(), Ceiling = new();
        public readonly List<int[]> Links = new();
        public readonly Dictionary<int, List<int>> ByColumn = new();
        public int[] Distance = Array.Empty<int>();
    }

    static readonly int[] Dx = { 1, -1, 0, 0 }, Dy = { 0, 0, 1, -1 };

    static OpenSet BuildOpen()
    {
        var open = new OpenSet();
        for (var c = 0; c < columns.Length; c++)
        {
            var list = columns[c];
            if (list == null) continue;
            for (var i = 0; i < list.Count; i++)
            {
                if (!list[i].Walk) continue;
                var id = open.Floor.Count;
                open.Column.Add(c);
                open.Floor.Add(list[i].Max);
                open.Ceiling.Add(i + 1 < list.Count ? list[i + 1].Min : int.MaxValue / 2);
                open.Links.Add(new[] { -1, -1, -1, -1 });
                if (!open.ByColumn.TryGetValue(c, out var ids))
                    open.ByColumn[c] = ids = new List<int>(1);
                ids.Add(id);
            }
        }
        var climb = (int)(Climb / Ch);
        var need = (int)Math.Ceiling(AgentHeight / Ch);
        for (var id = 0; id < open.Floor.Count; id++)
        {
            var c = open.Column[id];
            int x = c % width, y = c / width;
            for (var d = 0; d < 4; d++)
            {
                int nx = x + Dx[d], ny = y + Dy[d];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                if (!open.ByColumn.TryGetValue(nx + ny * width, out var others)) continue;
                var best = -1;
                foreach (var o in others)
                {
                    if (Math.Abs(open.Floor[o] - open.Floor[id]) > climb) continue;
                    var gap = Math.Min(open.Ceiling[o], open.Ceiling[id]) - Math.Max(open.Floor[o], open.Floor[id]);
                    if (gap < need) continue;
                    if (best < 0 || Math.Abs(open.Floor[o] - open.Floor[id]) < Math.Abs(open.Floor[best] - open.Floor[id]))
                        best = o;
                }
                open.Links[id][d] = best;
            }
        }
        Console.WriteLine($"Begehbare Saeulen: {open.Floor.Count}");
        return open;
    }

    // Abstand zum Rand in Zellen (Breitensuche), wie rcErodeWalkableArea nur mit vier Nachbarn.
    static void Erode(OpenSet open)
    {
        var dist = new int[open.Floor.Count];
        Array.Fill(dist, int.MaxValue);
        var queue = new Queue<int>();
        for (var id = 0; id < dist.Length; id++)
            if (open.Links[id].Any(l => l < 0))
            {
                dist[id] = 0;
                queue.Enqueue(id);
            }
        while (queue.Count > 0)
        {
            var id = queue.Dequeue();
            foreach (var o in open.Links[id])
                if (o >= 0 && dist[o] > dist[id] + 1)
                {
                    dist[o] = dist[id] + 1;
                    queue.Enqueue(o);
                }
        }
        open.Distance = dist;
    }

    // ---------- 75-cm-Raster ----------

    static void WriteGrid(OpenSet open, string target)
    {
        var need = (int)Math.Ceiling(AgentRadius / Cs);
        bool Inside(int id) => open.Distance[id] >= need;
        // Je 75-cm-Zelle: zusammenhaengende Teile der begehbaren Flaeche (innerhalb der Zelle verbunden) = ein Punkt.
        var cellOf = new Dictionary<int, (int ix, int iy)>();
        var groups = new Dictionary<(int, int), List<List<int>>>();
        var groupOf = new Dictionary<int, (int ix, int iy, int g)>();
        for (var id = 0; id < open.Floor.Count; id++)
        {
            if (!Inside(id)) continue;
            var c = open.Column[id];
            var wx = ox + (c % width + 0.5) * Cs;
            var wy = oy + (c / width + 0.5) * Cs;
            cellOf[id] = ((int)Math.Round(wx / Grid), (int)Math.Round(wy / Grid));
        }
        foreach (var id in cellOf.Keys)
        {
            if (groupOf.ContainsKey(id)) continue;
            var cell = cellOf[id];
            if (!groups.TryGetValue(cell, out var list))
                groups[cell] = list = new List<List<int>>();
            var members = new List<int> { id };
            groupOf[id] = (cell.ix, cell.iy, list.Count);
            for (var k = 0; k < members.Count; k++)
                foreach (var o in open.Links[members[k]])
                    if (o >= 0 && cellOf.TryGetValue(o, out var oc) && oc == cell && !groupOf.ContainsKey(o))
                    {
                        groupOf[o] = (cell.ix, cell.iy, list.Count);
                        members.Add(o);
                    }
            list.Add(members);
        }
        // Vertreter: die Saeule nahe der Zellmitte; Hoehe als Boden in Weltkoordinaten.
        var rep = new Dictionary<(int, int, int), (double x, double y, int z)>();
        foreach (var (cell, list) in groups)
            for (var g = 0; g < list.Count; g++)
            {
                var best = -1;
                var bestScore = double.MaxValue;
                foreach (var id in list[g])
                {
                    var c = open.Column[id];
                    var wx = ox + (c % width + 0.5) * Cs;
                    var wy = oy + (c / width + 0.5) * Cs;
                    var score = Math.Abs(wx - cell.Item1 * Grid) + Math.Abs(wy - cell.Item2 * Grid) - open.Distance[id] * 0.5;
                    if (score < bestScore) { bestScore = score; best = id; }
                }
                var bc = open.Column[best];
                rep[(cell.Item1, cell.Item2, g)] = (ox + (bc % width + 0.5) * Cs, oy + (bc / width + 0.5) * Cs,
                    (int)Math.Round(oz + open.Floor[best] * Ch));
            }
        // Verbindungen: Gruppen in Nachbarzellen, die sich ueber eine begehbare Saeule beruehren.
        var links = new Dictionary<(int, int, int), (int, int, int)?[]>();
        foreach (var key in rep.Keys)
            links[key] = new (int, int, int)?[4];
        foreach (var (id, g) in groupOf)
            foreach (var o in open.Links[id])
            {
                if (o < 0 || !groupOf.TryGetValue(o, out var og)) continue;
                var ddx = og.ix - g.ix;
                var ddy = og.iy - g.iy;
                var d = ddx == 1 && ddy == 0 ? 0 : ddx == -1 && ddy == 0 ? 1 : ddx == 0 && ddy == 1 ? 2 : ddx == 0 && ddy == -1 ? 3 : -1;
                if (d < 0) continue;
                var from = (g.ix, g.iy, g.g);
                var to = (og.ix, og.iy, og.g);
                var current = links[from][d];
                if (current == null || Math.Abs(rep[to].z - rep[from].z) < Math.Abs(rep[current.Value].z - rep[from].z))
                    links[from][d] = to;
            }
        // Gleiche gerundete Hoehe in einer Zelle wuerde in der Mod zusammenfallen: einen Zentimeter versetzen.
        var used = new HashSet<(int, int, int)>();
        var heightOf = new Dictionary<(int, int, int), int>();
        foreach (var key in rep.Keys.OrderBy(k => k.Item1).ThenBy(k => k.Item2).ThenBy(k => k.Item3))
        {
            var z = rep[key].z;
            while (!used.Add((key.Item1, key.Item2, z))) z++;
            heightOf[key] = z;
        }
        var inv = CultureInfo.InvariantCulture;
        using var writer = new StreamWriter(target);
        var linked = 0;
        foreach (var (key, r) in rep)
        {
            var parts = new List<string> { key.Item1.ToString(inv), key.Item2.ToString(inv), heightOf[key].ToString(inv) };
            foreach (var l in links[key])
            {
                parts.Add(l == null ? "-" : heightOf[l.Value].ToString(inv));
                if (l != null) linked++;
            }
            parts.Add(Math.Round(r.x).ToString(inv));
            parts.Add(Math.Round(r.y).ToString(inv));
            writer.WriteLine(string.Join(' ', parts));
        }
        Console.WriteLine($"Punkte: {rep.Count}, Verbindungen {linked / 2}, geschrieben {target}");
    }
}
