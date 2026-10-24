// =====================================================================
//  NetDefenseApp.cs  -  网络路由与攻击防御模拟 (NexOS 原生 C# 托管壳)
// ---------------------------------------------------------------------
//  纯 NexOS.Forms 重写，剥离所有 Web 技术栈。遵循 MiniCLR 红线：
//   - 不使用 try-catch / 泛型 List<T> / float / 静态分配器；
//   - 所有数据池为固定大小实例数组（ctor 中一次性分配），每帧 Local
//     临时量随 Bump 堆回卷自动回收；日志用定长环形缓冲，避免字符串
//     无限增长。
//   - 数据包移动在 OnPaint 内的 Update() 手写实现（无独立线程）。
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    // -----------------------------------------------------------------
    //  数据模型：值类型结构体 + 固定数组，绝不 new List<T>
    // -----------------------------------------------------------------
    public struct Node
    {
        public int    id;        // 0 基节点编号 (== 数组下标)
        public int    x, y;      // 屏幕坐标
        public int    status;    // 0 正常 / 1 被攻击 / 2 防御中
        public string ip;        // 显示用 IP
        public int    vuln;      // 1 = 存在漏洞(可扩展 /ms17)
    }

    public struct Packet
    {
        public int    type;      // 0 正常 / 1 恶意
        public int    fromNode;  // 正常包起点
        public int    toNode;    // 正常包终点
        public int    tNode;     // 恶意包环绕的目标节点
        public int    sx, sy;    // 当前段起点
        public int    ex, ey;    // 当前段终点
        public int    cx, cy;    // 当前像素坐标
        public int    prog;      // 进度 0..1000 (定点)
        public int    speed;     // 每帧步进
        public int    ang;       // 恶意包方向索引 (0..15)
        public int    dist;      // 恶意包到目标距离
        public int    life;      // 恶意包剩余生命 0..1000
    }

    public class NetDefenseApp : App
    {
        // ---- 常量 ----
        const int MAX_NODES  = 12;
        const int MAX_PACKETS = 256;
        const int MAX_LOGS    = 10;
        const int ST_NORMAL = 0, ST_ATK = 1, ST_DEF = 2;
        const int PK_NORMAL = 0, PK_MAL = 1;

        const uint COL_BG_TOP = 0x0A0F1E;
        const uint COL_BG_BOT = 0x03050A;
        const uint COL_EDGE   = 0x0A6E8E;
        const uint COL_NODE   = 0x00C8C8;   // 正常 青
        const uint COL_ATK    = 0xFF3030;   // 被攻击 红
        const uint COL_DEF    = 0x3080FF;   // 防御 蓝
        const uint COL_PKT    = 0xFFD028;   // 正常包 黄
        const uint COL_MAL    = 0xFF2828;   // 恶意包 红
        const uint COL_LOG    = 0x80F0C0;
        const uint COL_PANEL  = 0x0A0E18;
        const uint COL_INPUT  = 0x111A22;
        const uint COL_ACC    = 0xFFE000;

        // ---- 固定大小数据池 (ctor 一次性分配，绝不增长) ----
        Node[]   nodes;
        int[]    edgesA, edgesB;
        int      edgeCount = 0;
        int      nodeCount = 0;
        Packet[] packs;
        int      packCount = 0;
        string[] logs;
        int      logHead = 0;               // 环形写指针
        int[]    DIRX, DIRY;                // 16 向单位向量 *1000
        int      rngState;

        // ---- 交互 / 输入状态 ----
        TBox cmd = null;                    // 指令输入框
        int  cmdFocus = 0;                  // 0 失焦 / 1 聚焦
        int  selNode  = -1;                 // 已选路由起点
        int  inited   = 0;                  // 惰性布局标记

        // 命令框矩形 (OnPaint 计算, OnClick 复用)
        int cmdX = 0, cmdY = 0, cmdW = 0, cmdH = 26;

        // =================================================================
        //  构造：所有堆分配集中于此；动画常驻以保证持续重绘
        // =================================================================
        public NetDefenseApp()
        {
            nodes   = new Node[MAX_NODES];
            edgesA  = new int[MAX_NODES * 2];
            edgesB  = new int[MAX_NODES * 2];
            packs   = new Packet[MAX_PACKETS];
            logs    = new string[MAX_LOGS];
            DIRX    = new int[16];
            DIRY    = new int[16];
            for (int i = 0; i < MAX_LOGS; i++) logs[i] = "";
            cmd = new TBox();

            // 16 向单位向量 (定点 *1000)，避免任何三角函数/float
            DIRX[0]=1000;  DIRY[0]=0;     DIRX[1]=924;   DIRY[1]=383;
            DIRX[2]=707;   DIRY[2]=707;   DIRX[3]=383;   DIRY[3]=924;
            DIRX[4]=0;     DIRY[4]=1000;  DIRX[5]=-383;  DIRY[5]=924;
            DIRX[6]=-707;  DIRY[6]=707;   DIRX[7]=-924;  DIRY[7]=383;
            DIRX[8]=-1000; DIRY[8]=0;     DIRX[9]=-924;  DIRY[9]=-383;
            DIRX[10]=-707; DIRY[10]=-707; DIRX[11]=-383; DIRY[11]=-924;
            DIRX[12]=0;    DIRY[12]=-1000;DIRX[13]=383;  DIRY[13]=-924;
            DIRX[14]=707;  DIRY[14]=-707; DIRX[15]=924;  DIRY[15]=-383;

            rngState = Host.TickMs() | 1;
            Host.SetAnim(1);                // 关键：让 gui_tick 持续重绘
            Log("SYS", "NetDefense 已启动 - 8 节点在线");
        }

        public override string GetTitle() { return "NetDefense"; }

        // =================================================================
        //  伪随机 (LCG)：无 System.Random，避免堆扰动
        // =================================================================
        int Rnd()
        {
            rngState = (rngState * 1103515245 + 12345) & 0x7FFFFFFF;
            return rngState;
        }

        // =================================================================
        //  日志：定长环形缓冲，写入即覆盖最旧项，字符串数量恒有界
        // =================================================================
        void Log(string tag, string msg)
        {
            string line = U.Cat(U.Cat("[", tag), U.Cat("] ", msg));
            logs[logHead] = line;
            logHead++;
            if (logHead >= MAX_LOGS) logHead = 0;
        }

        // =================================================================
        //  惰性布局：首次 OnPaint 时按实际屏幕尺寸铺节点与连线
        // =================================================================
        void InitNet()
        {
            int W = Gfx.Width();
            int H = Gfx.Height();
            nodeCount = 8;
            int cols = 4, rows = 2;
            int areaL = 60, areaR = W - 60;
            int areaT = 70, areaB = H - 170;
            int cw = (areaR - areaL) / cols;
            int ch = (areaB - areaT) / rows;
            for (int i = 0; i < nodeCount; i++)
            {
                int c = i % cols, r = i / cols;
                int x = areaL + c * cw + cw / 2 + ((Rnd() % 40) - 20);
                int y = areaT + r * ch + ch / 2 + ((Rnd() % 30) - 15);
                nodes[i].id = i;
                nodes[i].x = x; nodes[i].y = y;
                nodes[i].status = ST_NORMAL;
                nodes[i].ip = U.Cat("10.0.0.", U.I(i + 1));
                nodes[i].vuln = (i < 2) ? 1 : 0;   // 前两个预设漏洞
            }
            // 环 + 少量额外边，保证连通与密度
            for (int i = 0; i < nodeCount; i++)
            {
                edgesA[edgeCount] = i;
                edgesB[edgeCount] = (i + 1) % nodeCount;
                edgeCount++;
            }
            if (nodeCount > 3) { edgesA[edgeCount]=0; edgesB[edgeCount]=3; edgeCount++; }
            if (nodeCount > 5) { edgesA[edgeCount]=1; edgesB[edgeCount]=5; edgeCount++; }
        }

        // =================================================================
        //  数据包池操作：追加或环形覆盖，绝不越界增长
        // =================================================================
        void AddPacket(Packet p)
        {
            if (packCount < MAX_PACKETS) { packs[packCount] = p; packCount++; }
            else { packs[Rnd() % MAX_PACKETS] = p; }   // 满则覆盖随机槽
        }
        void RemovePacket(int i)
        {
            packCount--;
            packs[i] = packs[packCount];               // 交换删除，O(1)
        }
        void SpawnNormal(int from, int to)
        {
            if (from < 0 || to < 0 || from >= nodeCount || to >= nodeCount)
            { Log("ERROR", "非法节点"); return; }
            if (from == to) { Log("WARN", "起点与终点相同"); return; }
            Packet p;
            p.type = PK_NORMAL; p.fromNode = from; p.toNode = to; p.tNode = -1;
            p.sx = nodes[from].x; p.sy = nodes[from].y;
            p.ex = nodes[to].x;   p.ey = nodes[to].y;
            p.prog = 0; p.speed = 10 + (Rnd() % 8);
            p.cx = p.sx; p.cy = p.sy;
            p.ang = 0; p.dist = 0; p.life = 0;
            AddPacket(p);
            Log("INFO", U.Cat(U.Cat("路由 ", nodes[from].ip), U.Cat(" -> ", nodes[to].ip)));
        }
        void SpawnMalicious(int target, int count)
        {
            for (int k = 0; k < count; k++)
            {
                Packet p;
                p.type = PK_MAL; p.fromNode = -1; p.toNode = -1; p.tNode = target;
                p.ang = Rnd() % 16;
                p.dist = 40 + (Rnd() % 40);
                p.life = 700 + (Rnd() % 500);
                p.speed = 4 + (Rnd() % 5);
                p.prog = 0; p.sx = 0; p.sy = 0; p.ex = 0; p.ey = 0;
                p.cx = 0; p.cy = 0;
                AddPacket(p);
            }
        }

        // =================================================================
        //  模拟步进：每帧在 OnPaint 调用；仅遍历定长数组，无新分配
        // =================================================================
        void Update()
        {
            int i = 0;
            while (i < packCount)
            {
                if (packs[i].type == PK_NORMAL)
                {
                    packs[i].prog += packs[i].speed;
                    if (packs[i].prog >= 1000)
                    {
                        int to = packs[i].toNode;
                        if (to >= 0 && to < nodeCount) Log("OK", U.Cat("数据包抵达 ", nodes[to].ip));
                        RemovePacket(i);
                        continue;
                    }
                    int f = packs[i].prog;
                    packs[i].cx = packs[i].sx + ((packs[i].ex - packs[i].sx) * f) / 1000;
                    packs[i].cy = packs[i].sy + ((packs[i].ey - packs[i].sy) * f) / 1000;
                }
                else // 恶意包：向目标节点收束，生命耗尽则在边缘重生
                {
                    packs[i].dist -= packs[i].speed;
                    packs[i].life -= 12;
                    if (packs[i].dist <= 4 || packs[i].life <= 0)
                    {
                        packs[i].dist = 40 + (Rnd() % 40);
                        packs[i].ang = Rnd() % 16;
                        packs[i].life = 700 + (Rnd() % 500);
                    }
                    int t = packs[i].tNode;
                    if (t >= 0 && t < nodeCount)
                    {
                        packs[i].cx = nodes[t].x + (DIRX[packs[i].ang] * packs[i].dist) / 1000;
                        packs[i].cy = nodes[t].y + (DIRY[packs[i].ang] * packs[i].dist) / 1000;
                    }
                }
                i++;
            }
        }

        // =================================================================
        //  指令解析 (宿主侧 TBox -> 直接改 Node 状态)
        // =================================================================
        int SpaceAt(string s)
        {
            int i = 0;
            while (i < s.Length) { if ((int)s[i] == 32) return i; i++; }
            return -1;
        }
        int StartsWith(string s, string pre)
        {
            int i = 0;
            while (i < pre.Length)
            {
                if (i >= s.Length) return 0;
                if ((int)s[i] != (int)pre[i]) return 0;
                i++;
            }
            return 1;
        }
        string ArgAfter(string s, int sp)
        {
            string r = "";
            int i = sp + 1;
            while (i < s.Length) { r = U.Cat(r, Host.CharStr((int)s[i])); i++; }
            return r;
        }
        int Atoi(string s)
        {
            int v = 0, i = 0;
            while (i < s.Length)
            {
                int c = (int)s[i];
                if (c >= 48 && c <= 57) v = v * 10 + (c - 48);
                else if (v > 0) break;
                i++;
            }
            return v;
        }
        void ExecCmd()
        {
            string t = cmd.text;
            if (t.Length == 0) return;
            Log("CMD", U.Cat("> ", t));
            int sp = SpaceAt(t);
            if (StartsWith(t, "/attack") == 1)
            {
                if (sp < 0) { Log("ERROR", "用法: /attack [ID]"); }
                else
                {
                    int id = Atoi(ArgAfter(t, sp));
                    if (id >= 0 && id < nodeCount)
                    {
                        nodes[id].status = ST_ATK;
                        SpawnMalicious(id, 30);       // 大量红色恶意包
                        Log("WARN", U.Cat(nodes[id].ip, " 遭受 DDoS 流量冲击!"));
                    }
                    else Log("ERROR", "未找到节点");
                }
            }
            else if (StartsWith(t, "/defend") == 1)
            {
                if (sp < 0) { Log("ERROR", "用法: /defend [ID]"); }
                else
                {
                    int id = Atoi(ArgAfter(t, sp));
                    if (id >= 0 && id < nodeCount)
                    {
                        nodes[id].status = ST_DEF;
                        int k = 0;
                        while (k < packCount)         // 清除打向该节点的恶意包
                        {
                            if (packs[k].type == PK_MAL && packs[k].tNode == id) RemovePacket(k);
                            else k++;
                        }
                        Log("OK", U.Cat(nodes[id].ip, " 防护罩展开，恶意流量被拦截"));
                    }
                    else Log("ERROR", "未找到节点");
                }
            }
            else if (StartsWith(t, "/help") == 1)
            {
                Log("SYS", "/attack [ID]  /defend [ID]  /help");
            }
            else Log("WARN", "未知指令");
            cmd.text = ""; cmd.cursor = 0; cmd.selA = 0; cmd.selB = 0;
        }

        // =================================================================
        //  输入
        // =================================================================
        public override void OnKey(int ch)
        {
            if (cmdFocus == 1)
            {
                if (ch == 27) { cmdFocus = 0; return; }                       // Esc 失焦
                if (ch == 10 || ch == 13) { ExecCmd(); cmdFocus = 0; return; }// Enter 执行
                cmd.Key(ch);                                                  // 其余进 TBox
            }
        }

        public override void OnClick(int mx, int my)
        {
            // 命中命令框 -> 聚焦输入
            if (mx >= cmdX && mx <= cmdX + cmdW && my >= cmdY && my <= cmdY + cmdH)
            { cmdFocus = 1; return; }
            // 命中节点
            int hit = -1;
            for (int i = 0; i < nodeCount; i++)
            {
                int dx = mx - nodes[i].x, dy = my - nodes[i].y;
                if (dx * dx + dy * dy <= 28 * 28) { hit = i; break; }
            }
            if (hit < 0) { selNode = -1; return; }
            if (selNode < 0) { selNode = hit; Log("INFO", U.Cat("选中起点 ", nodes[hit].ip)); }
            else if (hit != selNode) { SpawnNormal(selNode, hit); selNode = -1; } // A->B 建包
            else { selNode = -1; }
        }

        // =================================================================
        //  渲染 (Gfx 原语；无 Gfx.Clear，用 Gradient/FillRect 清屏)
        // =================================================================
        public override void OnPaint()
        {
            int W = Gfx.Width();
            int H = Gfx.Height();
            if (inited == 0) { InitNet(); inited = 1; }

            cmdX = 12; cmdY = H - 40; cmdW = W - 24; cmdH = 26;

            Update();   // 模拟步进 (每帧)

            // 背景
            Gfx.Gradient(0, 0, W, H, COL_BG_TOP, COL_BG_BOT);
            int gx = 0;
            while (gx < W) { Gfx.DrawLine(gx, 0, gx, H, 0x0A1A2A); gx += 40; }
            int gy = 0;
            while (gy < H) { Gfx.DrawLine(0, gy, W, gy, 0x0A1A2A); gy += 40; }

            // 标题 / 提示
            Gfx.Text(16, 16, "NetDefense - 网络路由与攻击防御模拟", 0x00E0E0);
            Gfx.Text(16, 38, "点击两节点路由 | 输入框 /attack [ID] | /defend [ID] | /help", 0x3A7A8A);

            // 网线
            for (int e = 0; e < edgeCount; e++)
            {
                int a = edgesA[e], b = edgesB[e];
                Gfx.DrawLine(nodes[a].x, nodes[a].y, nodes[b].x, nodes[b].y, COL_EDGE);
            }

            // 节点
            for (int i = 0; i < nodeCount; i++)
            {
                uint col = COL_NODE;
                if (nodes[i].status == ST_ATK) col = COL_ATK;
                else if (nodes[i].status == ST_DEF) col = COL_DEF;
                Gfx.FillCircle(nodes[i].x, nodes[i].y, 16, col);
                Gfx.DrawCircle(nodes[i].x, nodes[i].y, 22, col);
                if (selNode == i) Gfx.DrawCircle(nodes[i].x, nodes[i].y, 28, COL_ACC);
                Gfx.Text(nodes[i].x - 24, nodes[i].y + 28, nodes[i].ip, 0x8AD0E0);
            }

            // 数据包
            for (int i = 0; i < packCount; i++)
            {
                uint c = (packs[i].type == PK_NORMAL) ? COL_PKT : COL_MAL;
                Gfx.FillRect(packs[i].cx - 3, packs[i].cy - 3, 6, 6, c);
            }

            // 控制台面板 + 日志 (环形输出，最旧在上)
            int consoleY = H - 150, consoleH = 142;
            Gfx.FillRect(8, consoleY, W - 16, consoleH, COL_PANEL);
            Gfx.DrawRect(8, consoleY, W - 16, consoleH, 0x0A6E8E);
            int li = 0, idx = logHead;
            for (int k = 0; k < MAX_LOGS; k++)
            {
                Gfx.Text(16, consoleY + 6 + li * 13, logs[idx], COL_LOG);
                li++; idx++;
                if (idx >= MAX_LOGS) idx = 0;
            }

            // 命令输入框
            Gfx.FillRect(cmdX, cmdY, cmdW, cmdH, COL_INPUT);
            uint border = (cmdFocus == 1) ? COL_ACC : 0x0A6E8E;
            Gfx.DrawRect(cmdX, cmdY, cmdW, cmdH, border);
            Gfx.Text(cmdX + 8, cmdY + 6, U.Cat("> ", cmd.text), 0x00E0E0);
            if (cmdFocus == 1)
            {
                int cw2 = Gfx.Measure(cmd.text);
                Gfx.FillRect(cmdX + 8 + cw2 + 2, cmdY + 5, 8, 16, COL_ACC);   // 光标
            }
        }
    }
}
