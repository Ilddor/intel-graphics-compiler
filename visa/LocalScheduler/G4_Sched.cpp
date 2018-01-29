#include "LocalScheduler_G4IR.h"
#include "../GraphColor.h"
#include <iostream>
#include <functional>

using namespace vISA;
using namespace std;

namespace {

// Forward declaration.
class preNode;

class preEdge {
public:
    preEdge(preNode* N, DepType Ty)
        : mNode(N)
        , mType(Ty)
    {
    }

    preNode* getNode() const { return mNode; }
    DepType getType() const { return mType; }
    bool isDataDep() const
    {
        switch (mType) {
        case DepType::RAW:
        case DepType::WAR:
        case DepType::WAW:
        case DepType::RAW_MEMORY:
        case DepType::WAR_MEMORY:
        case DepType::WAW_MEMORY:
            return true;
        default:
            break;
        }
        return false;
    }

private:
    // Node at the end of this edge.
    preNode* mNode;

    // Type of dependence (RAW, WAW, WAR, etc.).
    DepType mType;
};

class preNode {
public:
    // A node for an instruction.
    preNode(G4_INST* Inst, unsigned ID)
        : Inst(Inst)
        , ID(ID)
    {
        setBarrier();
    }

    // A special node without attaching to an instruction.
    preNode()
    {
        setBarrier();
    }

    ~preNode();

    void* operator new(size_t sz, Mem_Manager& m) { return m.alloc(sz); }

    DepType getBarrier() const { return Barrier; }
    void setBarrier();

    vISA::G4_INST* getInst() const { return Inst; }
    unsigned getID() const { return ID; }

    typedef std::vector<preEdge>::iterator pred_iterator;
    typedef std::vector<preEdge>::const_iterator const_pred_iterator;
    pred_iterator pred_begin() { return Preds.begin(); }
    pred_iterator pred_end() { return Preds.end(); }
    bool pred_empty() const { return Preds.empty(); }
    unsigned pred_size() const { return (unsigned)Preds.size(); }
    std::vector<preEdge>& preds() { return Preds; }

    typedef std::vector<preEdge>::iterator succ_iterator;
    typedef std::vector<preEdge>::const_iterator const_succ_iterator;
    succ_iterator succ_begin() { return Succs.begin(); }
    succ_iterator succ_end() { return Succs.end(); }
    bool succ_empty() const { return Succs.empty(); }
    unsigned succ_size() const { return (unsigned)Succs.size(); }
    std::vector<preEdge>& succs() { return Succs; }

    void setTupleLead(preNode* Lead)
    {
        TupleLead = Lead;
        if (this == Lead)
            TupleParts = 1;
        else
            ++Lead->TupleParts;
    }
    preNode* getTupleLead() const { return TupleLead; }
    unsigned getTupleParts() const
    {
        if (this == TupleLead)
            return TupleParts;
        return TupleLead->TupleParts;
    }

    void print(std::ostream& os) const;
    void dump() const;

private:
    /* The following data shall not be overwritten by a scheduler. */
    std::vector<preEdge> Succs;
    std::vector<preEdge> Preds;

    // The corresponding instruction to this node.
    G4_INST* Inst = nullptr;

    // The unique node ID.
    unsigned ID = 0xFFFFFFFF;

    // Indicates whether this node is a barrier (NONE, SEND, CONTROL)
    DepType Barrier;

    /* The following data may be overwritten by a scheduler. */

    // Tuple node, which should be schedule in pair with this node.
    preNode *TupleLead = nullptr;
    unsigned TupleParts = 0;

    // # of preds not scheduled.
    unsigned NumPredsLeft = 0;

    // # of succs not scheduled.
    unsigned NumSuccsLeft = 0;

    // True once scheduled.
    bool isScheduled = false;
    bool isClustered = false;
    bool isClusterLead = false;

    friend class preDDD;
    friend class BB_Scheduler;
    friend class SethiUllmanQueue;
    friend class LatencyQueue;
};

// The dependency graph for a basic block.
class preDDD {
    Mem_Manager& mem;
    G4_Kernel& kernel;

    // The basic block to be scheduled.
    G4_BB* m_BB;

    // If this DDD has been built.
    bool IsDagBuilt = false;

    // All nodes to be built and scheduled.
    std::vector<preNode*> SNodes;

    // Special node for the schedule region.
    preNode EntryNode;
    preNode ExitNode;

    // New operands to be added into live ones.
    // This auxiliary vector is built while processing one node.
    std::vector<std::pair<preNode*, Gen4_Operand_Number>> NewLiveOps;

public:
    preDDD(Mem_Manager& m, G4_Kernel& kernel, G4_BB* BB)
        : mem(m)
        , kernel(kernel)
        , m_BB(BB)
    {
    }
    ~preDDD();

    Mem_Manager& getMem() const { return mem; }
    G4_Kernel& getKernel() const { return kernel; }
    G4_BB* getBB() const { return m_BB; }
    Options* getOptions() const { return kernel.getOptions(); }
    preNode* getEntryNode() { return &EntryNode; }
    preNode* getExitNode() { return &ExitNode; }
    std::vector<preNode*>& getNodes() { return SNodes; }

    // Build the data dependency graph.
    void buildGraph();

    // Initialize or clear per node state so that data dependency graph
    // could be used for scheduling.
    void reset();

    // Dump the DDD into a dot file.
    void dumpDagDot();

    // Each instruction creates live nodes for adding dependency edges.
    struct LiveNode {
        LiveNode(preNode* N, Gen4_Operand_Number OpNum)
            : N(N)
            , OpNum(OpNum)
        {
        }

        // The DAG node that this node belongs to.
        preNode* N;

        // This indicates which operand this node is tracking.
        Gen4_Operand_Number OpNum;

        // Check if this is a read/write operand.
        bool isWrite() const
        {
            return OpNum == Gen4_Operand_Number::Opnd_dst ||
                   OpNum == Gen4_Operand_Number::Opnd_condMod ||
                   OpNum == Gen4_Operand_Number::Opnd_implAccDst;
        }
        bool isRead() const { return !isWrite(); }

        friend void swap(LiveNode& a, LiveNode& b)
        {
            std::swap(a.N, b.N);
            std::swap(a.OpNum, b.OpNum);
        }
    };

private:
    // Keep live nodes while scanning the block.
    // Each declare is associated with a list of live nodes.
    std::unordered_map<const G4_Declare*, std::vector<LiveNode>> LiveNodes;

    // Use an extra list to track send message dependency.
    std::vector<preNode*> LiveSends;

    // The most recent scheduling barrier.
    preNode* prevBarrier = nullptr;


    // The core function for building the DAG.
    // This adds node to the DAG and adds any required edges
    // by checking the dependencies against the live nodes.
    void addNodeToGraph(preNode* N);

    // Create a new edge from pred->succ of type D.
    void addEdge(preNode* pred, preNode* succ, DepType D)
    {
        auto fn = [=](const preEdge& E) { return E.getNode() == succ; };
        if (pred->succ_end() == std::find_if(pred->succ_begin(), pred->succ_end(), fn)) {
            pred->Succs.emplace_back(succ, D);
            succ->Preds.emplace_back(pred, D);
        }
    }

    void processBarrier(preNode* curNode, DepType Dep);
    void processSend(preNode* curNode);
    void processReadWrite(preNode* curNode);
    void prune();
};

// Track and recompute register pressure for a block.
struct RegisterPressure
{
    PointsToAnalysis* p2a = nullptr;
    GlobalRA* gra = nullptr;
    LivenessAnalysis* liveness = nullptr;
    RPE* rpe = nullptr;
    G4_Kernel& kernel;
    Mem_Manager& mem;

    RegisterPressure(G4_Kernel& kernel, Mem_Manager& mem, RPE* rpe)
        : kernel(kernel)
        , mem(mem)
        , rpe(rpe)
    {
        // Initialize rpe if not available.
        if (rpe == nullptr) {
            init();
        } else {
            liveness = const_cast<LivenessAnalysis*>(rpe->getLiveness());
            rpe->run();
        }
    }

    ~RegisterPressure()
    {
        // Delete only if owns the following objects.
        if (p2a) {
            delete p2a;
            delete gra;
            delete liveness;
            delete rpe;
        }
    }

    void init()
    {
        p2a = new PointsToAnalysis(kernel.Declares, kernel.fg.getNumBB());
        p2a->doPointsToAnalysis(kernel.fg);
        gra = new GlobalRA(kernel, kernel.fg.builder->phyregpool, *p2a);
        liveness = new LivenessAnalysis(*gra, G4_GRF | G4_ADDRESS | G4_INPUT | G4_FLAG,
                                        /*doIPA*/ true, /*verifyRA*/ false);
        liveness->computeLiveness(true);
        rpe = new RPE(*gra, liveness);
        rpe->run();
    }

    bool isLiveOut(G4_BB* bb, G4_Declare* Dcl) const
    {
        G4_RegVar *V = Dcl->getRegVar();
        return liveness->isLiveAtExit(bb, V->getId());
    }

    void recompute(G4_BB *BB)
    {
        rpe->runBB(BB);
    }

    // Return the register pressure in GRF for an instruction.
    unsigned getPressure(G4_INST* Inst) const
    {
        return rpe->getRegisterPressure(Inst);
    }

    // Return the max pressure in GRFs for this block.
    unsigned getPressure(G4_BB* bb, std::vector<G4_INST*>* Insts = nullptr)
    {
        unsigned Max = 0;
        for (auto Inst : bb->instList) {
            if (Inst->isPseudoKill())
                continue;
            unsigned Pressure = rpe->getRegisterPressure(Inst);
            if (Pressure > Max) {
                Max = Pressure;
                if (Insts) {
                    Insts->clear();
                    Insts->push_back(Inst);
                }
            } else if (Pressure == Max && Insts) {
                Insts->push_back(Inst);
            }
        }

        return Max;
    }

    void dump(G4_BB *bb, const char *prefix = "")
    {
        unsigned Max = 0;
        std::vector<G4_INST *> Insts;
        for (auto Inst : bb->instList) {
            if (Inst->isPseudoKill()) {
                std::cerr << "[---] ";
                Inst->dump();
                continue;
            }
            unsigned Pressure = rpe->getRegisterPressure(Inst);
            if (Pressure > Max) {
                Max = Pressure;
                Insts.clear();
                Insts.push_back(Inst);
            } else if (Pressure == Max) {
                Insts.push_back(Inst);
            }
            std::cerr << "[" << Pressure << "] ";
            Inst->dump();
        }
        std::cerr << prefix << "the max pressure is " << Max << "\n";
        std::cerr << "Max pressure instructions are: \n";
        for (auto Inst : Insts) {
            Inst->dump();
        }
        std::cerr << "\n\n";
    }
};

struct SchedConfig
{
    enum {
        MASK_DUMP         = 1U << 0,
        MASK_LATENCY      = 1U << 1,
        MASK_SETHI_ULLMAN = 1U << 2,
        MASK_CLUSTTERING  = 1U << 3,
    };
    unsigned Dump : 1;
    unsigned UseLatency : 1;
    unsigned UseSethiUllman : 1;
    unsigned DoClustering : 1;

    explicit SchedConfig(unsigned Config)
        : Dump((Config & MASK_DUMP) != 0)
        , UseLatency((Config & MASK_LATENCY) != 0)
        , UseSethiUllman((Config & MASK_SETHI_ULLMAN) != 0)
        , DoClustering((Config & MASK_CLUSTTERING) != 0)
    {
    }
};

#define SCHED_DUMP(X)      \
    do {                   \
        if (config.Dump) { \
            X;             \
        }                  \
    } while (0)

static const unsigned SMALL_BLOCK_SIZE = 10;
static const unsigned PRESSURE_REDUCTION_THRESHOLD = 110;
static const unsigned PRESSURE_REDUCTION_MIN_BENEFIT = 5;
static const unsigned LATENCY_PRESSURE_THRESHOLD = 100;

// Scheduler on a single block.
class BB_Scheduler {
    // The kernel this block belongs to.
    G4_Kernel& kernel;

    // The data dependency graph for this block.
    preDDD& ddd;

    // Register pressure estimation and tracking.
    RegisterPressure& rp;

    // The schedule result.
    std::vector<G4_INST*> schedule;

    // Options to customize scheduler.
    SchedConfig config;

public:
    BB_Scheduler(G4_Kernel& kernel, preDDD& ddd, RegisterPressure& rp, SchedConfig config)
        : kernel(kernel)
        , ddd(ddd)
        , rp(rp)
        , config(config)
    {
    }

    void* operator new(size_t sz, Mem_Manager& m) { return m.alloc(sz); }

    Mem_Manager& getMem() const { return ddd.getMem(); }
    G4_Kernel& getKernel() const { return kernel; }
    G4_BB* getBB() const { return ddd.getBB(); }

    // Run list scheduling.
    void scheduleBlockForPressure() { SethiUllmanScheduling(); }
    void scheduleBlockForLatency() { LatencyScheduling(); }

    // Commit this scheduling if it reduces register pressure.
    bool commitIfBeneficial(unsigned &MaxRPE, bool IsTopDown);

private:
    void SethiUllmanScheduling();
    void LatencyScheduling();
    bool verifyScheduling();
};

} // namespace

preRA_Scheduler::preRA_Scheduler(G4_Kernel& k, Mem_Manager& m, RPE* rpe)
    : kernel(k)
    , mem(m)
    , rpe(rpe)
    , m_options(kernel.getOptions())
{
}

preRA_Scheduler::~preRA_Scheduler() {}

bool preRA_Scheduler::run()
{
    unsigned Threshold = m_options->getuInt32Option(vISA_preRA_ScheduleThreshold);
    unsigned SchedCtrl = m_options->getuInt32Option(vISA_preRA_ScheduleCtrl);
    SchedConfig config(SchedCtrl);
    RegisterPressure rp(kernel, mem, rpe);
    bool Changed = false;

    for (auto bb : kernel.fg.BBs) {
        if (bb->instList.size() < SMALL_BLOCK_SIZE) {
            SCHED_DUMP(std::cerr << "Skip block with instructions "
                << bb->instList.size() << "\n");
            continue;
        }

        unsigned MaxPressure = rp.getPressure(bb);
        if (MaxPressure <= Threshold && !config.UseLatency) {
            SCHED_DUMP(std::cerr << "Skip block with rp " << MaxPressure << "\n");
            continue;
        }

        SCHED_DUMP(rp.dump(bb, "Before scheduling, "));
        preDDD ddd(mem, kernel, bb);
        BB_Scheduler S(kernel, ddd, rp, config);

        if (MaxPressure >= Threshold && config.UseSethiUllman) {
            ddd.buildGraph();
            S.scheduleBlockForPressure();
            if (S.commitIfBeneficial(MaxPressure, /*IsTopDown*/ false)) {
                SCHED_DUMP(rp.dump(bb, "After scheduling for presssure, "));
                Changed = true;
            }
        }

        if (MaxPressure < LATENCY_PRESSURE_THRESHOLD && config.UseLatency) {
            ddd.reset();
            S.scheduleBlockForLatency();
            if (S.commitIfBeneficial(MaxPressure, /*IsTopDown*/ true)) {
                SCHED_DUMP(rp.dump(bb, "After scheduling for latency, "));
                Changed = true;
            }
        }
    }

    return Changed;
}

bool BB_Scheduler::verifyScheduling()
{
    std::set<G4_INST*> Insts;
    for (auto Inst : getBB()->instList)
        Insts.insert(Inst);

    for (auto Inst : schedule) {
        if (Insts.count(Inst) != 1) {
            Inst->dump();
            return false;
        }
    }

    return true;
}

namespace {

// The base class that implements common functionalities used in
// different scheduling algorithms.
class QueueBase {
protected:
    // The data-dependency graph.
    preDDD& ddd;

    // Registre pressure related data.
    RegisterPressure& rp;

    // Options to customize scheduler.
    SchedConfig config;

    // Ready nodes.
    std::vector<preNode*> Q;

    QueueBase(preDDD& ddd, RegisterPressure& rp, SchedConfig config)
        : ddd(ddd)
        , rp(rp)
        , config(config)
    {
    }

    virtual ~QueueBase() {}

public:
    preNode* getCurrTupleLead() const
    {
        return TheCurrTupleLead;
    }
    void setCurrTupleLead(preNode* N)
    {
        assert(N->getInst()->getExecSize() == 8 ||
               N->getInst()->getExecSize() == 16);
        TheCurrTupleLead = N->getTupleLead();
        TheCurrTupleParts = N->getTupleParts();
    }
    void updateCurrTupleLead(preNode* N)
    {
        assert(TheCurrTupleLead != nullptr);
        assert(N->getTupleLead() == TheCurrTupleLead);
        TheCurrTupleParts--;
        if (TheCurrTupleParts == 0)
            TheCurrTupleLead = nullptr;
    }

protected:
    // The current (send) tuple lead.
    preNode *TheCurrTupleLead = nullptr;
    unsigned TheCurrTupleParts = 0;
};

// Queue for Sethi-Ullman scheduling to reduce register pressure.
class SethiUllmanQueue : public QueueBase {
    // Sethi-Ullman numbers.
    std::vector<unsigned> Numbers;

    // The clustering nodes.
    std::vector<preNode*> Clusterings;
    std::set<preNode*> Visited;

    // Scheduling in clustering mode.
    bool IsInClusteringMode = false;

public:
    SethiUllmanQueue(preDDD& ddd, RegisterPressure& rp, SchedConfig config)
        : QueueBase(ddd, rp, config)
    {
        init();
    }

    // Add a new ready node.
    void push(preNode* N)
    {
        // Clustering nodes have been added.
        if (N->isClustered && !N->isClusterLead)
        {
            assert(std::find(Clusterings.begin(), Clusterings.end(), N) !=
                   Clusterings.end());
        }
        else
        {
            Q.push_back(N);
        }
    }

    // Schedule the top node.
    preNode* pop()
    {
        return select();
    }

    bool empty() const
    {
        return Q.empty() && Clusterings.empty();
    }

private:
    // Initialize Sethi-Ullman numbers.
    void init();

    // Select next ready node to schedule.
    preNode* select();

    preNode* scheduleClusteringNode();

    // Compare two ready nodes and decide which one should be scheduled first.
    // Return true if N2 has a higher priority than N1, false otherwise.
    bool compare(preNode* N1, preNode* N2);

    // Compute the Sethi-Ullman number for a node.
    unsigned calculateSethiUllmanNumber(preNode* N);
};

} // namespace

// This implements the idea in the paper by Appel & Supowit:
//
// Generalizations of the Sethi-Ullman algorithm for register allocation
//
unsigned SethiUllmanQueue::calculateSethiUllmanNumber(preNode* N)
{
    assert(N->getID() < Numbers.size());
    unsigned CurNum = Numbers[N->getID()];
    if (CurNum != 0)
        return CurNum;

    std::vector<std::pair<preNode*, unsigned>> Preds;
    for (auto I = N->pred_begin(), E = N->pred_end(); I != E; ++I) {
        auto& Edge = *I;
        if (!Edge.isDataDep())
            continue;

        // Skip pseudo-kills as they are lifetime markers.
        auto DefInst = Edge.getNode()->getInst();
        if (DefInst && DefInst->isPseudoKill())
            continue;

        // Recurse on the predecessors.
        unsigned Num = calculateSethiUllmanNumber(Edge.getNode());
        Preds.emplace_back(Edge.getNode(), Num);
    }

    auto getDstByteSize = [&](preNode* Node) -> unsigned {
        G4_INST* Inst = Node->getInst();
        if (!Inst)
            return 0;
        G4_DstRegRegion* Dst = Inst->getDst();
        if (Dst && Dst->getTopDcl()) {
            // If a variable lives out, then there is no extra cost to hold the result.
            if (rp.isLiveOut(ddd.getBB(), Dst->getTopDcl()))
                return 0;
            return Dst->getTopDcl()->getByteSize();
        }
        return 0;
    };

    if (Preds.size() > 0) {
        std::sort(Preds.begin(), Preds.end(),
                  [](std::pair<preNode*, unsigned> lhs,
                     std::pair<preNode*, unsigned> rhs)
                  {
                      return lhs.second < rhs.second;
                  });
        CurNum = Preds[0].second;
        for (unsigned i = 1, e = (unsigned)Preds.size(); i < e; ++i) {
            unsigned DstSize = getDstByteSize(Preds[i].first);
            CurNum = std::max(CurNum + DstSize, Preds[i].second);
        }
        return CurNum + getDstByteSize(N);
    }

    // Leaf node.
    return std::max(1U, getDstByteSize(N));
}

void SethiUllmanQueue::init()
{
    auto& Nodes = ddd.getNodes();
    unsigned N = (unsigned)Nodes.size();
    Numbers.resize(N, 0);
    for (unsigned i = 0; i < N; ++i) {
        unsigned j = N - 1 - i;
        Numbers[j] = calculateSethiUllmanNumber(Nodes[j]);
    }

#if 0
    std::cerr << "\n\n";
    for (auto I = Nodes.rbegin(); I != Nodes.rend(); ++I) {
        std::cerr << "SU[" << Numbers[(*I)->getID()] << "] ";
        (*I)->getInst()->dump();
    }
    std::cerr << "\n\n";
#endif
}

// Compare two ready nodes and decide which one should be scheduled first.
// Return true if N2 has a higher priority than N1, false otherwise.
bool SethiUllmanQueue::compare(preNode* N1, preNode* N2)
{
    // TODO. Introduce heuristics before comparing SU numbers.
    assert(N1->getID() < Numbers.size());
    assert(N2->getID() < Numbers.size());
    assert(N1->getID() != N2->getID());

    // Pseudo kill always has higher priority.
    if (N1->getInst()->isPseudoKill())
        return false;

    // Prefer to unlock a pending clustering node.
    if (IsInClusteringMode) {
        // Only kick in when top clustering node is not ready.
        assert(!Clusterings.empty());
        preNode* Top = Clusterings.back();
        if (Top->NumSuccsLeft > 0) {
            for (auto& SuccN : Top->succs()) {
                if (SuccN.getNode() == N1)
                    return false;
                if (SuccN.getNode() == N2)
                    return true;
            }
        }
    }

    unsigned SU1 = Numbers[N1->getID()];
    unsigned SU2 = Numbers[N2->getID()];

    // This is a bottom-up scheduling. Smaller SU number means higher priority.
    if (SU1 < SU2)
        return false;

    if (SU1 > SU2)
        return true;

    // Otherwise, break tie with their IDs. Smaller ID means higher priority.
    return N1->getID() > N2->getID();
}

preNode* SethiUllmanQueue::scheduleClusteringNode()
{
    // Schedule clustering nodes first.
    if (IsInClusteringMode && !Clusterings.empty()) {
        // Pop off already scheduled node, if any.
        while (!Clusterings.empty() && Clusterings.back()->isScheduled)
            Clusterings.pop_back();

        // All are scheduled, ending clustering mode.
        if (Clusterings.empty()) {
            IsInClusteringMode = false;
            return nullptr;
        }

        // The next clustering node is not ready yet.
        preNode* Top = Clusterings.back();
        if (Top->NumSuccsLeft > 0)
            return nullptr;

        // The next clustering node is ready and not scheduled yet.
        Clusterings.pop_back();
        if (Clusterings.empty())
            IsInClusteringMode = false;
        return Top;
    }

    // The width limit of clustering.
    const unsigned CLUSTER_SIZE_MIN = 4;
    const unsigned CLUSTER_SIZE_MAX = 16;

    // Match clustering nodes.
    auto collectClustering = [&](preNode* Node, preNode* predNode) {
        unsigned SU = Numbers[Node->getID()];
        for (auto& E : predNode->succs()) {
            preNode* N = E.getNode();
            // Match nodes with the same SU numbers, may not be ready.
            if (!E.isDataDep() || Numbers[N->getID()] != SU || N->isScheduled)
                break;
            Clusterings.push_back(N);
        }

        // Check if the first matching is successful.
        if (unsigned(Clusterings.size()) == predNode->succ_size() &&
            unsigned(Clusterings.size()) >= CLUSTER_SIZE_MIN &&
            unsigned(Clusterings.size()) <= CLUSTER_SIZE_MAX)
            return true;

        // Check if the second matching is successful.
        if (unsigned(Q.size()) >= CLUSTER_SIZE_MIN) {
            Clusterings.clear();
            for (auto& E : predNode->succs()) {
                preNode* N = E.getNode();
                // Only match ready nodes.
                if (!E.isDataDep() || N->isScheduled || N->NumSuccsLeft)
                    break;
                Clusterings.push_back(N);
            }
            if (unsigned(Clusterings.size()) == predNode->succ_size() &&
                unsigned(Clusterings.size()) >= CLUSTER_SIZE_MIN &&
                unsigned(Clusterings.size()) <= CLUSTER_SIZE_MAX) {
                return true;
            }
        }

        Clusterings.clear();
        return false;
    };

    if (config.DoClustering && Clusterings.empty()) {
        for (auto Node : Q) {
            for (auto I = Node->pred_begin(), E = Node->pred_end(); I != E; ++I) {
                if (I->isDataDep()) {
                    preNode* predN = I->getNode();
                    if (!Visited.insert(predN).second)
                        continue;
                    if (collectClustering(Node, predN))
                        break;
                }
            }

            if (!Clusterings.empty()) {
                for (auto N : Clusterings)
                    N->isClustered = true;

                Q.erase(std::remove_if(Q.begin(), Q.end(),
                        [](preNode* N) { return N->isClustered; }),
                        Q.end());

                std::sort(Clusterings.begin(), Clusterings.end(),
                          [](preNode* A, preNode* B) { return A->getID() > B->getID(); });

                // We put the leading node back to the regular queue to
                // participate SU number comparison.
                preNode* Top = Clusterings.back();
                Top->isClusterLead = true;
                if (Top->NumSuccsLeft == 0) {
                    Clusterings.pop_back();
                    Q.push_back(Top);
                    return nullptr;
                }
                break;
            }
        }
    }

    return nullptr;
}

preNode* SethiUllmanQueue::select()
{
    if (auto Top = scheduleClusteringNode())
        return Top;

    assert(!Q.empty());
    auto TopIter = Q.end();
    for (auto I = Q.begin(), E = Q.end(); I != E; ++I) {
        preNode *N = *I;
        // If there's a node to be paired, skip send not in pair.
        if (N->getInst() && N->getInst()->isSend())
            if (TheCurrTupleLead && N->getTupleLead() != TheCurrTupleLead)
                continue;
        if (TopIter == Q.end() || compare(*TopIter, *I))
            TopIter = I;
    }
    assert(TopIter != Q.end());
    preNode* Top = *TopIter;
    std::swap(*TopIter, Q.back());
    Q.pop_back();

    // This selected node is clustered. From now on, schedule all
    // other clustered nodes.
    if (Top->isClustered) {
        IsInClusteringMode = true;
        return Top;
    }

    return Top;
}

// The basic idea is...
//
void BB_Scheduler::SethiUllmanScheduling()
{
    schedule.clear();
    SethiUllmanQueue Q(ddd, rp, config);
    Q.push(ddd.getExitNode());

    while (!Q.empty()) {
        preNode *N = Q.pop();
        assert(!N->isScheduled && N->NumSuccsLeft == 0);
        if (N->getInst() != nullptr) {
            if (N->getInst()->isSend() && N->getTupleLead()) {
                // If it's the pair of the current node, reset the node to be
                // paired. If it's send with pair, ensure its pair is scheduled
                // before other sends by setting the current node to be paired.
                if (!Q.getCurrTupleLead())
                    Q.setCurrTupleLead(N);
                if (Q.getCurrTupleLead())
                    Q.updateCurrTupleLead(N);
            }
            schedule.push_back(N->getInst());
            N->isScheduled = true;
        }

        for (auto I = N->pred_begin(), E = N->pred_end(); I != E; ++I) {
            preNode *Node = I->getNode();
            assert(!Node->isScheduled && Node->NumSuccsLeft);
            --Node->NumSuccsLeft;
            if (Node->NumSuccsLeft == 0)
                Q.push(Node);
        }
    }

    assert(verifyScheduling());
}

namespace {

// Queue for scheduling to hide latency.
class LatencyQueue : public QueueBase {
    // Assign a priority to each node.
    std::vector<unsigned> Priorities;

    // Map each instruction to a group ID. Instructions in the same
    // group will be scheduled for latency.
    std::map<G4_INST *, unsigned> GroupInfo;

public:
    LatencyQueue(preDDD& ddd, RegisterPressure& rp, SchedConfig config)
        : QueueBase(ddd, rp, config)
    {
        init();
    }

    // Add a new ready node.
    void push(preNode* N)
    {
        Q.push_back(N);
    }

    // Schedule the top node.
    preNode* pop()
    {
        return select();
    }

    bool empty() const
    {
        return Q.empty();
    }

private:
    void init();
    unsigned calculatePriority(preNode *N);

    preNode* select();

    // Compare two ready nodes and decide which one should be scheduled first.
    // Return true if N2 has a higher priority than N1, false otherwise.
    bool compare(preNode* N1, preNode* N2);
};

} // namespace

// Scheduling block to hide latency (top down).
//
void BB_Scheduler::LatencyScheduling()
{
    schedule.clear();
    LatencyQueue Q(ddd, rp, config);
    Q.push(ddd.getEntryNode());

    while (!Q.empty()) {
        preNode *N = Q.pop();
        assert(N->NumPredsLeft == 0);
        if (N->getInst() != nullptr) {
            if (N->getInst()->isSend() && N->getTupleLead()) {
                // If it's the pair of the current node, reset the node to be
                // paired. If it's send with pair, ensure its pair is scheduled
                // before other sends by setting the current node to be paired.
                if (!Q.getCurrTupleLead())
                    Q.setCurrTupleLead(N);
                if (Q.getCurrTupleLead())
                    Q.updateCurrTupleLead(N);
            }
            schedule.push_back(N->getInst());
            N->isScheduled = true;
        }

        for (auto I = N->succ_begin(), E = N->succ_end(); I != E; ++I) {
            preNode *Node = I->getNode();
            assert(!Node->isScheduled && Node->NumPredsLeft);
            --Node->NumPredsLeft;
            if (Node->NumPredsLeft == 0)
                Q.push(Node);
        }
    }

    assert(verifyScheduling());
}

static void mergeSegments(const std::vector<unsigned>& RPtrace,
                          const std::vector<unsigned>& Max,
                          const std::vector<unsigned>& Min,
                          std::vector<unsigned>& Segments)
{
    unsigned n = std::min<unsigned>((unsigned)Max.size(), (unsigned)Min.size());
    assert(n >= 2);

    // Starts with a local minimum.
    //
    //  C        /\
    //          /  \
    //  A \    /    \
    //  D  \  /      \
    //  B   \/
    //
    if (Min[0] < Max[0]) {
        unsigned Hi = RPtrace[0];
        unsigned Lo = RPtrace[Min[0]];

        for (unsigned i = 1; i < n; ++i) {
            unsigned Hi2 = RPtrace[Max[i - 1]];
            unsigned Lo2 = RPtrace[Min[i]];

            if ((Hi2 - Lo + Hi) <= LATENCY_PRESSURE_THRESHOLD) {
                Hi = Hi2 - Lo + Hi;
                Lo = Lo2;
            } else {
                // Do not merge two segments, and end the last group.
                Segments.push_back(Min[i - 1]);
                Hi = Hi2;
                Lo = Lo2;
            }
        }

        // If ends with a local maximal, then Hi2 is the last max;
        // otherwise it is the end pressure.
        unsigned Hi2 = (Min.back() > Max.back()) ? RPtrace.back()
                                                 : RPtrace[Max.back()];
        if ((Hi2 - Lo + Hi) > LATENCY_PRESSURE_THRESHOLD)
            Segments.push_back(Min.back());
        return;
    }

    // Starts with local maximum.
    //
    //
    //  D             /\
    //               /  \
    //  B     /\    /    \
    //       /  \  /      \
    //  C   /    \/
    //     /
    //  A /
    //
    unsigned Hi = RPtrace[Max[0]];
    unsigned Lo = RPtrace[Min[0]];
    for (unsigned i = 1; i < n; ++i) {
        unsigned Hi2 = RPtrace[Max[i]];
        unsigned Lo2 = RPtrace[Min[i]];

        // Merge two segments
        if ((Hi2 - Lo + Hi) <= LATENCY_PRESSURE_THRESHOLD) {
            Hi = Hi2 - Lo + Hi;
            Lo = Lo2;
        } else {
            // Do not merge two segments, and end the last group.
            Segments.push_back(Min[i - 1]);
            Hi = Hi2;
            Lo = Lo2;
        }
    }

    // If ends with a local maximal, then Hi2 is the last max;
    // otherwise it is the end pressure.
    unsigned Hi2 = (Min.back() > Max.back()) ? RPtrace.back()
                                             : RPtrace[Max.back()];
    if ((Hi2 - Lo + Hi) > LATENCY_PRESSURE_THRESHOLD)
        Segments.push_back(Min.back());
}

void LatencyQueue::init()
{
    G4_BB* BB = ddd.getBB();
    assert(rp.getPressure(BB) < LATENCY_PRESSURE_THRESHOLD);

    // Scan block forward and group instructions, without causing 
    // excessive pressure increase. First collect the register
    // pressure trace in this block.
    std::vector<unsigned> RPtrace;
    RPtrace.reserve(BB->instList.size());
    for (auto Inst : BB->instList)
        RPtrace.push_back(rp.getPressure(Inst));

    // Collect all local maximum and minimum.
    std::vector<unsigned> Max, Min;
    bool IsIncreasing = true;
    for (unsigned i = 1; i + 1 < RPtrace.size(); ++i) {
        if (RPtrace[i] > RPtrace[i-1])
            IsIncreasing = true;
        else if (RPtrace[i] < RPtrace[i - 1])
            IsIncreasing = false;

        if (RPtrace[i] > RPtrace[i - 1] && RPtrace[i] > RPtrace[i + 1])
            Max.push_back(i);
        else if (IsIncreasing && RPtrace[i] == RPtrace[i - 1] && RPtrace[i] > RPtrace[i + 1])
            Max.push_back(i);
        else if (RPtrace[i] < RPtrace[i - 1] && RPtrace[i] < RPtrace[i + 1])
            Min.push_back(i);
        else if (!IsIncreasing && RPtrace[i] == RPtrace[i - 1] && RPtrace[i] < RPtrace[i + 1])
            Min.push_back(i);
    }

    if (Max.size() <= 1 || Min.size() <= 1) {
        // Simple case, there is only a single group.
        for (auto Inst : BB->instList)
            GroupInfo[Inst] = 0;
    } else {
        // Multiple high/low pressure segments. We merge consective segments
        // without breaking rpe limit. E.g. given [A, B, C, D, E] with A < B >
        // C and C < D > E, B, D are local maximum and C is a local minimum.
        // We merge [A, B, C] with [C, D, E] when B + D - C <= THRESHOLD,
        // resulting a larger segment [A, B + D - C, E]. Approximately,
        // B + D - C bounds the maximal pressure in this merged segment [A, E].
        // Otherwise, do not merge [A, C] with [C, E], C ends the current group,
        // and starts a new group.
        //
        std::vector<unsigned> Segments;
        mergeSegments(RPtrace, Max, Min, Segments);

        // Iterate segments and assign a group id to each insstruction.
        unsigned i = 0;
        unsigned j = 0;
        for (auto Inst : BB->instList) {
            if (j >= Segments.size()) {
                GroupInfo[Inst] = j;
            } else if (i < Segments[j]) {
                GroupInfo[Inst] = j;
            } else {
                GroupInfo[Inst] = j;
                ++j;
            }
            ++i;
        }
    }

    auto& Nodes = ddd.getNodes();
    unsigned N = (unsigned)Nodes.size();
    Priorities.resize(N, 0);
    for (unsigned i = 0; i < N; ++i)
        Priorities[i] = calculatePriority(Nodes[i]);

#if 0
    std::cerr << "\n\n";
    for (auto I = Nodes.rbegin(); I != Nodes.rend(); ++I) {
        std::cerr << "(GroupId, Priority, RPE) = ("
                  << std::setw(2)<< GroupInfo[(*I)->getInst()]
                  << ", " << std::setw(4) << Priorities[(*I)->getID()]
                  << ", " << std::setw(3)<< rp.getPressure((*I)->getInst())
                  << ") ";
        (*I)->getInst()->dump();
    }
    std::cerr << "\n\n";
#endif
}

unsigned LatencyQueue::calculatePriority(preNode* N)
{
    G4_INST* Inst = N->getInst();
    if (!Inst)
        return 0;

    assert(N->getID() < Priorities.size());
    unsigned CurPriority = Priorities[N->getID()];
    if (CurPriority > 0)
        return CurPriority;

    unsigned Priority = 0;
    for (auto I = N->succ_begin(), E = N->succ_end(); I != E; ++I) {
        // Recurse on the successors.
        auto& Edge = *I;
        unsigned SuccPriority = calculatePriority(Edge.getNode());
        unsigned Latency = 0;

        if (Inst && !Inst->isPseudoKill() && Edge.isDataDep()) {
            Latency = UNCOMPR_LATENCY;
            switch (Edge.getType()) {
            case RAW:
            case RAW_MEMORY:
            case WAW:
                if (Inst->isSend()) {
                    if (G4_SendMsgDescriptor* MsgDesc = Inst->getMsgDesc())
                        Latency = MsgDesc->getFFLatency();
                } else if (Inst->isMath()) {
                    Latency = EDGE_LATENCY_MATH;
                    if (Inst->asMathInst()->getMathCtrl() == MATH_FDIV ||
                        Inst->asMathInst()->getMathCtrl() == MATH_POW)
                        Latency = EDGE_LATENCY_MATH_TYPE2;
                } else {
                    Latency = IVB_PIPELINE_LENGTH;
                }
                break;
            default:
                break;
            }
        }

        Priority = std::max(Priority, SuccPriority + Latency);
    }

    return Priority;
}

preNode* LatencyQueue::select()
{
    assert(!Q.empty());
    auto TopIter = Q.end();
    for (auto I = Q.begin(), E = Q.end(); I != E; ++I) {
        preNode* N = *I;
        // If there's a node to be paired, skip send not in pair.
        if (N->getInst() && N->getInst()->isSend())
            if (TheCurrTupleLead && N->getTupleLead() != TheCurrTupleLead)
                continue;
        if (TopIter == Q.end() || compare(*TopIter, *I))
            TopIter = I;
    }
    assert(TopIter != Q.end());
    preNode* Top = *TopIter;
    std::swap(*TopIter, Q.back());
    Q.pop_back();
    return Top;
}

// Compare two ready nodes and decide which one should be scheduled first.
// Return true if N2 has a higher priority than N1, false otherwise.
bool LatencyQueue::compare(preNode* N1, preNode* N2)
{
    assert(N1->getID() != N2->getID());
    assert(N1->getInst() && N2->getInst());

    G4_INST* Inst1 = N1->getInst();
    if (Inst1->isPseudoKill()) {
        // Direction is top-down, we compare the underlying nodes.
        assert(!N1->succ_empty());
        preNode *N1F = N1->Succs.front().getNode();
        // If the underlying node is not ready yet, then this pseudo-kill
        // shall not be scheduled ahead of N2.
        if (N1F->NumPredsLeft > 1)
            return true;
        return compare(N1F, N2);
    }

    G4_INST* Inst2 = N2->getInst();
    if (Inst2->isPseudoKill()) {
        // Direction is top-down, we compare the underlying nodes.
        assert(!N2->succ_empty());
        preNode *N2F = N2->Succs.front().getNode();
        // If the underlying node is not ready yet, then this pseudo-kill
        // shall not be scheduled ahead of N1.
        if (N2F->NumPredsLeft > 1)
            return false;
        return compare(N1, N2F);
    }

    // Group ID has higher priority, smaller ID means higher priority.
    unsigned GID1 = GroupInfo[N1->getInst()];
    unsigned GID2 = GroupInfo[N2->getInst()];
    if (GID1 > GID2)
        return true;
    if (GID1 < GID2)
        return false;

    // Within the same group, compare their priority.
    unsigned P1 = Priorities[N1->getID()];
    unsigned P2 = Priorities[N2->getID()];
    if (P2 > P1)
        return true;
    if (P1 > P2)
        return false;

    // Flavour sends.
    if (Inst1->isSend() && !Inst2->isSend())
        return false;
    else if (!Inst1->isSend() && Inst2->isSend())
        return true;

    // Otherwise, break tie on ID.
    // Larger ID means higher priority.
    return N2->getID() > N1->getID();
}

// Commit this scheduling if it is better.
bool BB_Scheduler::commitIfBeneficial(unsigned& MaxRPE, bool IsTopDown)
{
    INST_LIST& CurInsts = getBB()->instList;
    if (schedule.size() != CurInsts.size()) {
        SCHED_DUMP(std::cerr << "schedule reverted due to mischeduling.\n\n");
        return false;
    }
    if (std::equal(CurInsts.begin(), CurInsts.end(), schedule.rbegin())) {
        SCHED_DUMP(std::cerr << "schedule not committed due to no change.\n\n");
        return false;
    }

    // Note that list::swap does not work for some reason, but list::splice works.
    INST_LIST TempInsts;
    TempInsts.splice(TempInsts.begin(), CurInsts, CurInsts.begin(), CurInsts.end());
    assert(CurInsts.empty());

    // evaluate this scheduling.
    if (IsTopDown)
        for (auto Inst : schedule)
            CurInsts.push_back(Inst);
    else
        for (auto Inst : schedule)
            CurInsts.push_front(Inst);

    rp.recompute(getBB());
    unsigned NewRPE = rp.getPressure(getBB());
    if (config.UseLatency && IsTopDown) {
        // For hiding latency.
        if (NewRPE <= LATENCY_PRESSURE_THRESHOLD) {
            SCHED_DUMP(std::cerr << "schedule committed for latency.\n\n");
            MaxRPE = NewRPE;
            return true;
        } else {
            SCHED_DUMP(std::cerr << "the pressure is increased to " << NewRPE << "\n");
        }
    } else {
        // For reducing rpe.
        if (NewRPE + PRESSURE_REDUCTION_MIN_BENEFIT <= MaxRPE) {
            bool AbortOnSpill = kernel.getOptions()->getOption(vISA_AbortOnSpill);
            if (kernel.getSimdSize() == 32 && AbortOnSpill) {
                // It turns out that simd32 kernels may be scheduled like slicing, which
                // in general hurts latency hidding. If not insist to compile for simd32,
                // make rp reduction conservative.
                //
                if (NewRPE < LATENCY_PRESSURE_THRESHOLD) {
                    SCHED_DUMP(std::cerr << "schedule committed with reduced pressure.\n\n");
                    MaxRPE = NewRPE;
                    return true;
                }
            } else {
                SCHED_DUMP(std::cerr << "schedule committed with reduced pressure.\n\n");
                MaxRPE = NewRPE;
                return true;
            }
        } else if (NewRPE < MaxRPE) {
            SCHED_DUMP(std::cerr << "the reduced pressure is " << NewRPE - MaxRPE << "\n");
        }
    }

    SCHED_DUMP(rp.dump(getBB(), "schedule reverted, "));
    CurInsts.clear();
    CurInsts.splice(CurInsts.begin(), TempInsts, TempInsts.begin(), TempInsts.end());
    return false;
}

// Implementation of preNode.
preNode::~preNode() {}

void preNode::setBarrier()
{
    // Check if there is an indirect operand in this instruction.
    auto hasIndirectOpnd = [=](G4_INST* Inst) {
        G4_DstRegRegion* dst = Inst->getDst();
        if (dst && dst->isIndirect())
            return true;
        for (auto opNum : { Opnd_src0, Opnd_src1, Opnd_src2 }) {
            G4_Operand* opnd = Inst->getOperand(opNum);
            if (opnd && opnd->isSrcRegRegion() &&
                opnd->asSrcRegRegion()->isIndirect())
                return true;
        }
        return false;
    };

    if (Inst == nullptr)
        Barrier = DepType::OPT_BARRIER;
    else if (Inst->isLabel())
        Barrier = DepType::DEP_LABEL;
    else if (hasIndirectOpnd(Inst))
        Barrier = DepType::INDIRECT_ADDR_BARRIER;
    else if (Inst->isFence())
        Barrier = DepType::OPT_BARRIER;
    else
        Barrier = CheckBarrier(Inst);
}

void preNode::print(ostream& os) const
{
    os << "ID: " << this->ID << "\n";
    Inst->emit(os, false, false);

    os << "Preds: ";
    for (auto& E : this->Preds)
        os << E.getNode()->ID << ",";
    os << "\n";

    os << "Succs: ";
    for (auto& E : this->Preds)
        os << E.getNode()->ID << ",";
    os << "\n";
}

void preNode::dump() const { print(std::cerr); }

// Implementation of preDDD.
preDDD::~preDDD()
{
    for (auto X : SNodes) {
        X->~preNode();
    }
}

// Build the data dependency bottom up with two simple
// special nodes.
void preDDD::buildGraph()
{
     assert(!IsDagBuilt);

    // Starts with the exit node.
    addNodeToGraph(&ExitNode);

    INST_LIST& Insts = m_BB->instList;
    unsigned NumOfInsts = (unsigned)Insts.size();
    SNodes.reserve(NumOfInsts);

    auto I = Insts.rbegin(), E = Insts.rend();
    for (unsigned i = 0; I != E; ++I) {
        preNode* N = new (mem) preNode(*I, i++);
        SNodes.push_back(N);
        addNodeToGraph(N);
    }

    // Ends with the entry node.
    addNodeToGraph(&EntryNode);

    // prune the graph.
    prune();

    // Set DAG is complete.
    IsDagBuilt = true;

    // Initialize perNode data.
    reset();
}

void preDDD::addNodeToGraph(preNode *N)
{
    NewLiveOps.clear();
    DepType Dep = N->getBarrier();
    if (Dep != DepType::NODEP)
    {
        processBarrier(N, Dep);
    }
    else
    {
        assert(N->Inst && "not an instruction");
        processSend(N);
        processReadWrite(N);
    }

    // Adding live node should happen in the end, as illustrated below:
    // add X X 1
    // add Y X 2
    for (auto& Item : NewLiveOps) {
        preNode* N = std::get<0>(Item);
        Gen4_Operand_Number OpNum = std::get<1>(Item);
        G4_Operand* Opnd = N->getInst()->getOperand(OpNum);
        assert(Opnd != nullptr);
        G4_Declare* Dcl = Opnd->getTopDcl();
        LiveNodes[Dcl].emplace_back(N, OpNum);
    }

    // Update live nodes on sends.
    G4_INST* Inst = N->getInst();
    if (Inst && Inst->isSend()) {
        assert(!Inst->getMsgDesc()->isScratchRW());
        LiveSends.push_back(N);
    }


    // No explicit dependency found, so it depends on previous barrier.
    if (N->succ_empty() && N != prevBarrier) {
        assert(prevBarrier && "out of sync");
        addEdge(N, prevBarrier, prevBarrier->getBarrier());
    }
}

void preDDD::processBarrier(preNode* curNode, DepType Dep)
{
    // A barrier kills all live nodes, so add dependency edge to all live
    // nodes and clear.
    for (auto& Nodes : LiveNodes) {
        for (LiveNode& X : Nodes.second) {
            if (X.N->pred_empty()) {
                addEdge(curNode, X.N, Dep);
            }
        }
        Nodes.second.clear();
    }
    for (auto N : LiveSends) {
        if (N->pred_empty()) {
            addEdge(curNode, N, Dep);
        }
    }
    LiveSends.clear();

    // Add an edge when there is no edge on previous barrier.
    if (prevBarrier != nullptr && prevBarrier->pred_empty())
        addEdge(curNode, prevBarrier, Dep);
    prevBarrier = curNode;

    G4_INST *Inst = curNode->getInst();
    if (Inst == nullptr)
        return;

    for (auto OpNum :
        { Gen4_Operand_Number::Opnd_dst, Gen4_Operand_Number::Opnd_src0,
          Gen4_Operand_Number::Opnd_src1, Gen4_Operand_Number::Opnd_src2,
          Gen4_Operand_Number::Opnd_src3, Gen4_Operand_Number::Opnd_pred,
          Gen4_Operand_Number::Opnd_condMod, Gen4_Operand_Number::Opnd_implAccSrc,
          Gen4_Operand_Number::Opnd_implAccDst }) {
        G4_Operand* opnd = Inst->getOperand(OpNum);
        if (opnd == nullptr || opnd->getBase() == nullptr || opnd->isNullReg())
            continue;
        NewLiveOps.emplace_back(curNode, OpNum);
    }
}

// - Remove one element from vector if pred is true.
// - Return the iterator to the next element.
template <typename T, typename AllocTy>
static typename std::vector<T, AllocTy>::iterator
kill_if(bool pred, std::vector<T, AllocTy>& Elts,
        typename std::vector<T, AllocTy>::iterator Iter)
{
    if (!pred)
        return std::next(Iter);

    assert(Iter != Elts.end());
    assert(!Elts.empty());
    // This is the last element so the next element is none.
    if (&*Iter == &Elts.back()) {
        Elts.pop_back();
        return Elts.end();
    }

    // This is not the last element, swap with the tail.
    // Keep the iterator unchanged.
    std::swap(*Iter, Elts.back());
    Elts.pop_back();
    return Iter;
}

// Compute {RAW,WAW,WAR,NODEP} for given operand to a live node.
//
static std::pair<DepType, G4_CmpRelation>
getDep(G4_Operand* Opnd, const preDDD::LiveNode& LN)
{
    G4_CmpRelation Rel = G4_CmpRelation::Rel_undef;
    G4_Operand *Other = LN.N->getInst()->getOperand(LN.OpNum);
    if (Opnd->isDstRegRegion())
        Rel = Opnd->asDstRegRegion()->compareOperand(Other);
    else if (Opnd->isCondMod())
        Rel = Opnd->asCondMod()->compareOperand(Other);
    else if (Opnd->isSrcRegRegion())
        Rel = Opnd->asSrcRegRegion()->compareOperand(Other);
    else if (Opnd->isPredicate())
        Rel = Opnd->asPredicate()->compareOperand(Other);
    else
        Rel = Opnd->compareOperand(Other);

    // No dependency.
    if (Rel == G4_CmpRelation::Rel_disjoint)
        return std::make_pair(DepType::NODEP, Rel);

    DepType Deps[] = { DepType::NODEP, DepType::RAW, DepType::WAR, DepType::WAW };
    int i = int(LN.isWrite());
    int j = int(Opnd->isDstRegRegion() || Opnd->isCondMod());
    return std::make_pair(Deps[i * 2 + j], Rel);
}

// This is not a label nor a barrier and check the dependency
// introduced by this node.
void preDDD::processReadWrite(preNode* curNode)
{
    G4_INST* Inst = curNode->getInst();
    for (auto OpNum : { Gen4_Operand_Number::Opnd_dst,
                        Gen4_Operand_Number::Opnd_condMod,
                        Gen4_Operand_Number::Opnd_implAccDst }) {
        G4_Operand* opnd = Inst->getOperand(OpNum);
        if (opnd == nullptr || opnd->getBase() == nullptr || opnd->isNullReg())
            continue;
        G4_Declare* Dcl = opnd->getTopDcl();
        auto& Nodes = LiveNodes[Dcl];
        // Iterate all live nodes associated to the same declaration.
        for (auto Iter = Nodes.begin(); Iter != Nodes.end(); /*empty*/) {
            LiveNode& liveNode = *Iter;
            auto DepRel = getDep(opnd, liveNode);
            if (DepRel.first != DepType::NODEP) {
                addEdge(curNode, liveNode.N, DepRel.first);
                // Check if this kills current live node. If yes, remove it.
                bool pred = DepRel.second == G4_CmpRelation::Rel_eq ||
                            DepRel.second == G4_CmpRelation::Rel_gt;
                Iter = kill_if(pred, Nodes, Iter);
            } else
                ++Iter;
        }
        NewLiveOps.emplace_back(curNode, OpNum);
    }

    for (auto OpNum :
        { Gen4_Operand_Number::Opnd_src0, Gen4_Operand_Number::Opnd_src1,
          Gen4_Operand_Number::Opnd_src2, Gen4_Operand_Number::Opnd_src3,
          Gen4_Operand_Number::Opnd_pred, Gen4_Operand_Number::Opnd_implAccSrc }) {
        G4_Operand* opnd = curNode->getInst()->getOperand(OpNum);
        if (opnd == nullptr || opnd->getBase() == nullptr || opnd->isNullReg())
            continue;

        G4_Declare* Dcl = opnd->getTopDcl();
        auto& Nodes = LiveNodes[Dcl];
        // Iterate all live nodes associated to the same declaration.
        for (auto& liveNode : Nodes) {
            // Skip read live nodes.
            if (liveNode.isRead())
                continue;

            std::pair<DepType, G4_CmpRelation> DepRel = getDep(opnd, liveNode);
            if (DepRel.first != DepType::NODEP)
                addEdge(curNode, liveNode.N, DepRel.first);
        }
        NewLiveOps.emplace_back(curNode, OpNum);
    }
}

void preDDD::processSend(preNode* curNode)
{
    G4_INST* Inst = curNode->getInst();
    if (!Inst->isSend())
        return;

    assert(!Inst->getMsgDesc()->isScratchRW() && "not expected");
    for (auto Iter = LiveSends.begin(); Iter != LiveSends.end(); /*empty*/) {
        preNode* liveN = *Iter;
        DepType Dep = getDepSend(Inst, liveN->getInst(), getOptions());
        if (Dep != DepType::NODEP) {
            addEdge(curNode, liveN, Dep);
            // Check if this kills current live send. If yes, remove it.
            bool pred = (Dep == DepType::WAW_MEMORY || Dep == DepType::RAW_MEMORY);
            Iter = kill_if(pred, LiveSends, Iter);
        } else
            ++Iter;
    }
}

void preDDD::prune()
{
    auto removeEdge = [=](preNode* pred, preNode* succ) {
        auto Iter = std::find_if(pred->succ_begin(), pred->succ_end(),
            [=](preEdge& E) { return E.getNode() == succ; });
        if (Iter == pred->succ_end())
            return;
        kill_if(true, pred->Succs, Iter);
        Iter = std::find_if(succ->pred_begin(), succ->pred_end(),
            [=](preEdge& E) { return E.getNode() == pred; });
        assert(Iter != succ->pred_end());
        kill_if(true, succ->Preds, Iter);
    };

    // Currently only prune up to two levels.
    for (auto N : SNodes) {
        std::set<preNode*> Seen;
        for (auto& E1 : N->Succs)
            for (auto& E2 : E1.getNode()->Succs)
                Seen.insert(E2.getNode());

        for (auto T : Seen)
            removeEdge(N, T);
    }
}

// Reset states that a scheduler may overwrite.
void preDDD::reset()
{
    if (!IsDagBuilt)
        buildGraph();

    auto isHalfN = [](G4_INST* Inst, unsigned N) -> bool {
        return Inst->isSend() &&
               Inst->getExecSize() == 16 &&
               Inst->getMaskOffset() == N * 16;
    };

    auto isQuadN = [](G4_INST* Inst, unsigned N) -> bool {
        return Inst->isSend() &&
               Inst->getExecSize() == 8 &&
               Inst->getMaskOffset() == N * 8;
    };

    auto isTupleLead = [&isHalfN, &isQuadN](G4_INST* Inst) -> bool {
        return isHalfN(Inst, 1) || isQuadN(Inst, 3);
    };

    auto isTuplePart = [&isHalfN, &isQuadN](G4_INST* Inst) -> bool {
        return isHalfN(Inst, 0) || isHalfN(Inst, 1) || isQuadN(Inst, 0) ||
               isQuadN(Inst, 1) || isQuadN(Inst, 2) || isQuadN(Inst, 3);
    };

    // Mark SIMD32 send tuples.
    preNode* Lead = nullptr;
    for (auto N : SNodes) {
        G4_INST* Inst = N->getInst();
        if (!Inst)
            continue;
        if (isTupleLead(Inst)) {
            // In case, send tuples are interleaved, bail out.
            if (Lead)
                break;
            Lead = N;
            N->setTupleLead(Lead);
            continue;
        }
        if (Lead && isTuplePart(Inst)) {
            N->setTupleLead(Lead);
            if (Inst->getMaskOffset() == 0)
                Lead = nullptr;
        }
    }

    for (auto N : SNodes) {
        N->NumPredsLeft = unsigned(N->Preds.size());
        N->NumSuccsLeft = unsigned(N->Succs.size());
        N->isScheduled = false;
        N->isClustered = false;
        N->isClusterLead = false;
    }

    EntryNode.NumPredsLeft = 0;
    EntryNode.NumSuccsLeft = unsigned(EntryNode.Succs.size());
    EntryNode.isScheduled = false;
    EntryNode.isClustered = false;
    EntryNode.isClusterLead = false;

    ExitNode.NumPredsLeft = unsigned(ExitNode.Preds.size());
    ExitNode.NumSuccsLeft = 0;
    ExitNode.isScheduled = false;
    ExitNode.isClustered = false;
    ExitNode.isClusterLead = false;
}

void preDDD::dumpDagDot()
{
    const char* asmFileName = "nullasm";
    getOptions()->getOption(VISA_AsmFileName, asmFileName);
    string fileName(asmFileName);
    fileName.append(".bb")
            .append(std::to_string(getBB()->getId()))
            .append(".preDDD.dot");

    fstream ofile(fileName, ios::out);
    ofile << "digraph DAG {" << std::endl;

    for (auto N : SNodes) {
        // Node
        ofile << N->ID << "[label=\"";
        N->getInst()->emit(ofile);
        ofile << ", I" << N->getInst()->getLocalId() << "\"]\n";
        // Edge
        for (auto &E : N->Succs) {
            DepType depType = E.getType();
            const char *depColor, *depStr;
            std::tie(depColor, depStr)
                = (depType == RAW || depType == RAW_MEMORY) ? std::make_tuple("black", "RAW")
                : (depType == WAR || depType == WAR_MEMORY) ? std::make_tuple("red", "WAR")
                : (depType == WAW || depType == WAW_MEMORY) ? std::make_tuple("orange", "WAW")
                : std::make_tuple("grey", "other");

            // Example: 30->34[label="RAW",color="{red|black|yellow}"];
            ofile << N->ID << "->" << E.getNode()->ID
                << "[label=\"" << depStr << "\""
                << ",color=\"" << depColor << "\""
                << "];\n";
        }
    }

    ofile << "}\n";
    ofile.close();
}
