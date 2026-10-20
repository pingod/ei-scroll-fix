import math
def run(shim_on, notches=12, dy=-15.0, ddisc=-15):
    acc=[0.0,0.0]   # double[2] shared by both handlers (0=x,1=y)
    units=[]
    for _ in range(notches):
        s = dy/1.5 if shim_on else dy
        d = 0 if shim_on else ddisc
        # smooth handler (actual binary: *0.1, trunc, *120)
        a = acc[1] + s*0.1
        t = math.trunc(a)
        if t != 0:
            units.append(-t*120)
            acc[1] = a - t
        # discrete handler (actual binary: /120.0, trunc, *120), shares acc[1]
        b = acc[1] + d
        c = math.trunc(b/120.0)*120
        acc[1] = b - c
        if c != 0:
            units.append(-c)
    return units

def lines(u):  # mac fakeMouseWheel: (int)(u*3.0/120.0)
    return [int(u*3.0/120.0) for u in u]

for label,on in (("现状（未装垫片）",False),("装垫片后",True)):
    u=run(on); l=lines(u)
    print(f"{label}: EI单位={u[:10]}{'...' if len(u)>10 else ''}")
    print(f"{'':22} mac行数={l[:12]}  12格累计={sum(l)} 行")
    print()

# 长跑 1000 格，检查漂移/丢格
u=run(True,notches=1000)
print("1000 格（装垫片）: 事件数=",len(u)," 全为120? ",all(abs(x)==120 for x in u),
      " 累计单位=",sum(abs(x) for x in u),"(应为 1000*120 =",1000*120,")")
# 快速滚动合帧 -30
print("快速滚 2 格合帧 dy=-30 ->", run(True,notches=1,dy=-30.0,ddisc=-30), "（应=240）")
print("快速滚 6 格合帧 dy=-90 ->", run(True,notches=1,dy=-90.0,ddisc=-90), "（应=720）")
