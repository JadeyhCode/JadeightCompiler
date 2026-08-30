// codegen2.inc — 表达式代码生成（由 codegen.cpp 尾部 include，已在 namespace j8 内）
// 所有函数都是 CodeGen 的成员方法定义。

// ---------------- 辅助 ----------------
static VarSlot* findSlot(FuncInstance* inst, const std::string& name) {
    for (auto& [id, s] : inst->slots) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

static bool isComparison(BinaryOp op) {
    return op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt ||
           op == BinaryOp::Ge || op == BinaryOp::Eq || op == BinaryOp::Ne;
}

// ---------- dl_reg / dl_call：签名串 "rettype(arg1,arg2,...)" 解析 ----------
static bool dlParseSig(const std::string& s, std::string& ret, std::vector<std::string>& args) {
    size_t lp = s.find('(');
    if (lp == std::string::npos || s.empty() || s.back() != ')') return false;
    ret = s.substr(0, lp);
    std::string a = s.substr(lp + 1, s.size() - lp - 2);
    if (!a.empty()) {
        size_t start = 0;
        while (true) {
            size_t comma = a.find(',', start);
            args.push_back(a.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return true;
}
// 返回类型宽度的 .j8 表示；nullptr = void（无返回值）。ptr 按 u64 宽度处理。
static Type* dlTypeByName(const std::string& t) {
    if (t == "u8" || t == "i8") return Type::make(TypeKind::U8);
    if (t == "u16" || t == "i16") return Type::make(TypeKind::U16);
    if (t == "u32") return Type::make(TypeKind::U32);
    if (t == "i32") return Type::make(TypeKind::I32);
    if (t == "u64") return Type::make(TypeKind::U64);
    if (t == "i64") return Type::make(TypeKind::I64);
    if (t == "f64") return Type::make(TypeKind::F64);
    if (t == "ptr") return Type::make(TypeKind::U64);
    return nullptr;
}
// 前向声明：autoExternIndex 定义在文件后部
static int autoExternIndex(Sema* sema, const std::string& name);

static Type* commonArithType(Type* a, Type* b) {
    if (!a || !b) return Type::make(TypeKind::U64);
    auto norm = [](Type* t) -> Type* {
        if (t->kind == TypeKind::Bool || t->kind == TypeKind::Char) return Type::make(TypeKind::U8);
        if (t->kind == TypeKind::Ent) return Type::make(TypeKind::U32);
        return t;
    };
    a = norm(a);
    b = norm(b);
    if (a->isFloat() || b->isFloat()) return Type::make(TypeKind::F64);
    if (a->isInt() && b->isInt()) {
        int wa = typeWidth(a), wb = typeWidth(b);
        if (wa == wb) return (a->isSigned() == b->isSigned()) ? a : (a->isSigned() ? b : a);
        return wa > wb ? a : b;
    }
    return Type::make(TypeKind::U64);
}

int CodeGen::tempOffset(FuncInstance* inst, int k) { return inst->tempBase + 8 * k; }

void CodeGen::emitFunction(FuncInstance* inst, int idx, bool counting) {
    curFuncIdx_ = idx;
    if (!counting && getenv("J8_DEBUG_LAYOUT")) {
        fprintf(stderr, "[layout %s] retB=%d paramB=%d localB=%d userLocalB=%d tempBase=%d main=%d\n",
                inst->key.c_str(), inst->retBytes, inst->paramBytes, inst->localBytes,
                inst->userLocalBytes, inst->tempBase, (int)inst->isMain);
        for (auto& [id, vs] : inst->slots)
            fprintf(stderr, "    slot %d: name=%s off=%d w=%d param=%d\n", id, vs.name.c_str(), vs.offset, vs.width, (int)vs.isParam);
    }
    if (!counting) {
        em_.comment("=== function " + inst->key + " @ " + std::to_string(codeOff_[idx]) + " ===");
    }
    emitPrologue(inst);
    if (inst->compileBody) {
        if (inst->compileBody->kind == StmtKind::Block) {
            emitStmts(inst->compileBody->body, inst);
        } else {
            emitStmt(inst->compileBody, inst);
        }
    }
    emitReturn(inst);   // 兜底返回（不可达时无害）
}

// ---------------- 表达式 ----------------
void CodeGen::emitExpr(Expr* e, FuncInstance* inst) {
    if (!e) return;
    Type* t = e->type;
    switch (e->kind) {
        case ExprKind::IntLit: {
            int w = typeWidth(t);
            if (w <= 0) w = 8;
            pushConst(w, e->intVal);
            break;
        }
        case ExprKind::FloatLit: {
            uint64_t bits;
            std::memcpy(&bits, &e->floatVal, 8);
            pushConst(8, bits);
            break;
        }
        case ExprKind::BoolLit: case ExprKind::CharLit:
            pushConst(1, e->intVal);
            break;
        case ExprKind::NullLit:
            pushConst(8, 0);
            break;
        case ExprKind::StrLit: {
            int rel = stringRelOff_.count(e->strVal) ? stringRelOff_[e->strVal] : 0;
            uint64_t abs = static_cast<uint64_t>(dataStart_) + static_cast<uint64_t>(rel);
            pushReg(kBcBase, 8);
            ins(kRegMovi(VmNum::U64), "R1, " + std::to_string(abs));
            pushReg(1, 8);
            ins(OP_ADD_PTR);
            break;
        }
        case ExprKind::VarRef: {
            VarSlot* vs = findSlot(inst, e->name);
            if (!vs) { pushConst(8, 0); break; }
            if (t->isStruct() || t->kind == TypeKind::Ptr) {
                if (vs->isParam) { loadSlot(0, 8, vs->offset); pushReg(0, 8); }
                else if (t->kind == TypeKind::Ptr && vs->isArray) {
                    // 局部数组：值是首元素地址
                    ins(OP_LEA, "0, " + std::to_string(vs->offset)); em_.pushD(8);
                } else if (t->kind == TypeKind::Ptr) {
                    // 指针变量：值是槽内指针
                    loadSlot(0, 8, vs->offset);
                    pushReg(0, 8);
                } else {
                    // 结构体：值=地址
                    ins(OP_LEA, "0, " + std::to_string(vs->offset)); em_.pushD(8);
                }
            } else if (t->kind == TypeKind::Exist) {
                loadSlot(0, 8, vs->offset); pushReg(0, 8);
                loadSlot(0, 8, vs->offset + 8); pushReg(0, 8);
            } else {
                int w = typeWidth(t);
                if (w <= 0) w = 8;
                loadSlot(0, w, vs->offset);
                pushReg(0, w);
            }
            break;
        }
        case ExprKind::Unary: {
            switch (e->unOp) {
                case UnaryOp::Neg: {
                    // 先压 0，再压操作数，最后相减：避免 pushConst 复用 R0 覆盖 popReg 暂存值
                    int w = typeWidth(t);
                    if (w <= 0) w = 8;
                    pushConst(w, 0);
                    emitExpr(e->lhs.get(), inst);
                    ins(kSub(vmNum(t)));
                    break;
                }
                case UnaryOp::Pos:
                    emitExpr(e->lhs.get(), inst);
                    break;
                case UnaryOp::Not: {
                    emitExpr(e->lhs.get(), inst);
                    Type* lt = e->lhs->type;
                    if (lt->isFloat()) { pushConst(8, 0); ins(OP_CMP_EQ_F64); emitBoolNot(); }
                    else if (lt->kind == TypeKind::Ptr) { pushConst(8, 0); ins(OP_CMP_EQ_PTR); emitBoolNot(); }
                    else {
                        int w = typeWidth(lt);
                        if (w <= 0) w = 8;
                        pushConst(w, 0);
                        ins(opCmpEQ(vmNum(lt)));
                        emitBoolNot();
                    }
                    break;
                }
                case UnaryOp::BitNot: {
                    emitExpr(e->lhs.get(), inst);
                    ins(kNot(vmNum(t)));
                    break;
                }
                case UnaryOp::Deref: {
                    emitExpr(e->lhs.get(), inst);   // 地址
                    if (!t->isStruct() && t->kind != TypeKind::Exist) {
                        // 值 = load（结构体/存在类型：地址即值）
                        popReg(1, 8);
                        storeSlot(1, 8, tempOffset(inst, 0));
                        int w = typeWidth(t);
                        if (w <= 0) w = 8;
                        loadPtr(0, w, tempOffset(inst, 0));
                        pushReg(0, w);
                    }
                    break;
                }
                case UnaryOp::AddrOf:
                    emitAddress(e->lhs.get(), inst);
                    break;
                case UnaryOp::PreInc: case UnaryOp::PreDec:
                case UnaryOp::PostInc: case UnaryOp::PostDec: {
                    emitIncDec(e, inst);
                    break;
                }
            }
            break;
        }
        case ExprKind::Binary: {
            if (e->binOp == BinaryOp::LogAnd || e->binOp == BinaryOp::LogOr) {
                std::string lCont = newLabel("sc_a");
                std::string lSkip = newLabel("sc_b");
                std::string lEnd = newLabel("sc_end");
                emitBoolFlag(e->lhs.get(), inst);
                if (e->binOp == BinaryOp::LogAnd) {
                    branchOnFlag(lCont, lSkip);
                } else {
                    branchOnFlag(lSkip, lCont);
                }
                label(lCont);
                emitBoolFlag(e->rhs.get(), inst);
                jumpTo(lEnd);
                label(lSkip);
                pushConst(1, e->binOp == BinaryOp::LogAnd ? 0 : 1);
                label(lEnd);
                break;
            }
            if (isComparison(e->binOp)) {
                Type* common = commonArithType(e->lhs->type, e->rhs->type);
                emitExpr(e->lhs.get(), inst);
                coerceOnStack(e->lhs->type, common, inst);
                emitExpr(e->rhs.get(), inst);
                coerceOnStack(e->rhs->type, common, inst);
                emitCompare(e->binOp, common, inst);
            } else {
                emitExpr(e->lhs.get(), inst);
                coerceOnStack(e->lhs->type, t, inst);
                emitExpr(e->rhs.get(), inst);
                coerceOnStack(e->rhs->type, t, inst);
                emitBinaryOp(e->binOp, t, inst);
            }
            break;
        }
        case ExprKind::Ternary: {
            std::string lThen = newLabel("t_then");
            std::string lElse = newLabel("t_else");
            std::string lEnd = newLabel("t_end");
            emitBoolFlag(e->cond.get(), inst);
            branchOnFlag(lThen, lElse);
            label(lThen);
            emitExpr(e->thenExpr.get(), inst);
            coerceOnStack(e->thenExpr->type, t, inst);
            jumpTo(lEnd);
            label(lElse);
            emitExpr(e->elseExpr.get(), inst);
            coerceOnStack(e->elseExpr->type, t, inst);
            label(lEnd);
            break;
        }
        case ExprKind::Index: {
            emitAddress(e, inst);
            loadThroughAddress(t, inst);
            break;
        }
        case ExprKind::Member: {
            emitAddress(e, inst);
            loadThroughAddress(t, inst);
            break;
        }
        case ExprKind::Call:
            emitCallExpr(e, inst);
            break;
        case ExprKind::MethodCall: {
            if (e->resolved && e->resolved->isDynamicDispatch) {
                emitDynamicCall(e, inst);
            } else if (e->resolved && e->resolved->callee) {
                emitMethodCall(e->resolved->callee, e->lhs.get(), e->args, inst);
            } else {
                pushConst(8, 0);
            }
            break;
        }
        case ExprKind::GenericCall: {
            const std::string& name = e->tmplName;
            if (name == "get" || name == "has" || name == "add" || name == "remove" ||
                name == "run" || name == "spawn" || name == "despawn" || name == "alive" ||
                name == "entity_count") {
                emitEcs(e, inst, name, e->name);
            } else if (e->resolved && e->resolved->callee) {
                emitStaticCall(e->resolved->callee, e->args, inst);
            } else {
                pushConst(8, 0);
            }
            break;
        }
        case ExprKind::Cast: {
            emitExpr(e->lhs.get(), inst);
            coerceOnStack(e->lhs->type, t, inst);
            break;
        }
        case ExprKind::Sizeof:
            pushConst(4, e->intVal);
            break;
        case ExprKind::New:
            emitNew(e, inst);
            break;
        case ExprKind::Assign:
            emitAssign(e, inst);
            break;
        case ExprKind::CompoundAssign:
            emitCompoundAssign(e, inst);
            break;
        case ExprKind::IncDec:
            emitIncDec(e, inst);
            break;
        default:
            pushConst(8, 0);
            break;
    }
}

// 通过地址取值（地址在栈顶）
void CodeGen::loadThroughAddress(Type* t, FuncInstance* inst) {
    if (!t) { pushConst(8, 0); return; }
    if (t->isStruct() || t->kind == TypeKind::Void) return;   // 地址即值
    if (t->kind == TypeKind::Exist) {
        // [addr] → [data][vt]
        popReg(1, 8);
        storeSlot(1, 8, tempOffset(inst, 0));
        loadPtr(0, 8, tempOffset(inst, 0));
        pushReg(0, 8);
        // vt = *(addr+8)
        loadSlot(0, 8, tempOffset(inst, 0));
        pushReg(0, 8);
        pushConst(8, 8);
        ins(OP_ADD_PTR);
        popReg(0, 8);
        storeSlot(0, 8, tempOffset(inst, 1));
        loadPtr(0, 8, tempOffset(inst, 1));
        pushReg(0, 8);
        return;
    }
    int w = typeWidth(t);
    if (w <= 0) w = 8;
    popReg(1, 8);
    storeSlot(1, 8, tempOffset(inst, 0));
    loadPtr(0, w, tempOffset(inst, 0));
    pushReg(0, w);
}

// 取地址（压 u64 地址）
void CodeGen::emitAddress(Expr* e, FuncInstance* inst) {
    if (!e) { pushConst(8, 0); return; }
    switch (e->kind) {
        case ExprKind::VarRef: {
            VarSlot* vs = findSlot(inst, e->name);
            if (!vs) { pushConst(8, 0); break; }
            if (e->type && (e->type->isStruct() || e->type->kind == TypeKind::Ptr)) {
                if (vs->isParam) { loadSlot(0, 8, vs->offset); pushReg(0, 8); }
                else { ins(OP_LEA, "0, " + std::to_string(vs->offset)); em_.pushD(8); }
            } else if (e->type && e->type->kind == TypeKind::Exist) {
                ins(OP_LEA, "0, " + std::to_string(vs->offset)); em_.pushD(8);
            } else {
                ins(OP_LEA, "0, " + std::to_string(vs->offset)); em_.pushD(8);
            }
            break;
        }
        case ExprKind::Member: {
            if (e->compIdx >= 0) {
                // ECS 实体字段：world + dataOff(C) + e*size + fieldOff
                const std::string& compName = sema_->components[e->compIdx]->name;
                auto clIt = sema_->compLayout.find(compName);
                int dataOff = clIt != sema_->compLayout.end() ? clIt->second.dataOff : 0;
                int compSize = clIt != sema_->compLayout.end() ? clIt->second.size : 0;
                Decl* cd = sema_->structs[compName];
                int fieldOff = 0;
                if (cd) {
                    for (auto& f : cd->fields) {
                        if (f.name == e->name) { fieldOff = f.offset; break; }
                    }
                }
                worldAddr(static_cast<uint64_t>(dataOff));
                emitExpr(e->lhs.get(), inst);          // e (ent)
                popReg(0, 4);
                pushReg(0, 8);                          // e as u64
                if (compSize != 1) {
                    pushConst(8, static_cast<uint64_t>(compSize));
                    ins(OP_MUL_U64);
                }
                ins(OP_ADD_PTR);
                if (fieldOff != 0) {
                    pushConst(8, static_cast<uint64_t>(fieldOff));
                    ins(OP_ADD_PTR);
                }
                break;
            }
            // 指针左值：值即地址；结构体左值：取地址
            if (e->lhs->type && e->lhs->type->kind == TypeKind::Ptr) {
                emitExpr(e->lhs.get(), inst);
            } else {
                emitAddress(e->lhs.get(), inst);
            }
            // 结构体字段偏移
            Type* base = e->lhs->type;
            if (base->kind == TypeKind::Ptr && base->pointee &&
                base->pointee->kind == TypeKind::Struct) {
                base = base->pointee;
            }
            if (base->kind == TypeKind::Struct) {
                Decl* sd = sema_->structs[base->name];
                if (sd) {
                    for (auto& f : sd->fields) {
                        if (f.name == e->name) {
                            if (f.offset != 0) {
                                pushConst(8, static_cast<uint64_t>(f.offset));
                                ins(OP_ADD_PTR);
                            }
                            break;
                        }
                    }
                }
            }
            break;
        }
        case ExprKind::Unary:
            if (e->unOp == UnaryOp::Deref) emitExpr(e->lhs.get(), inst);
            else pushConst(8, 0);
            break;
        case ExprKind::Index: {
            if (e->lhs->type && e->lhs->type->kind == TypeKind::Ptr) {
                emitExpr(e->lhs.get(), inst);
            } else {
                emitAddress(e->lhs.get(), inst);
            }
            emitExpr(e->rhs.get(), inst);
            coerceOnStack(e->rhs->type, Type::make(TypeKind::U64), inst);
            Type* elem = e->lhs->type ? e->lhs->type->pointee : nullptr;
            int size = 1;
            if (elem) size = sema_->sizeOf(elem);
            if (size > 1) {
                pushConst(8, static_cast<uint64_t>(size));
                ins(OP_MUL_U64);
            }
            ins(OP_ADD_PTR);
            break;
        }
        default:
            // 非左值（字面量等）：压 0 兜底
            pushConst(8, 0);
            break;
    }
}

// 条件求值 → u8 0/1
void CodeGen::emitBoolFlag(Expr* e, FuncInstance* inst) {
    if (!e) { pushConst(1, 0); return; }
    if (e->kind == ExprKind::Binary && isComparison(e->binOp)) {
        emitExpr(e, inst);
        return;
    }
    if (e->kind == ExprKind::Unary && (e->unOp == UnaryOp::Not)) {
        emitExpr(e, inst);
        return;
    }
    if (e->kind == ExprKind::Binary &&
        (e->binOp == BinaryOp::LogAnd || e->binOp == BinaryOp::LogOr)) {
        emitExpr(e, inst);
        return;
    }
    emitExpr(e, inst);
    Type* t = e->type;
    if (t && t->isFloat()) {
        pushConst(8, 0);
        ins(OP_CMP_EQ_F64);
        emitBoolNot();
    } else if (t && t->kind == TypeKind::Ptr) {
        pushConst(8, 0);
        ins(OP_CMP_EQ_PTR);
        emitBoolNot();
    } else if (t && (t->isInt() || t->kind == TypeKind::Ent || t->kind == TypeKind::Bool ||
                     t->kind == TypeKind::Char)) {
        int w = typeWidth(t);
        if (w <= 0) w = 8;
        pushConst(w, 0);
        ins(opCmpEQ(vmNum(t)));
        emitBoolNot();
    } else {
        em_.popD(8);
        pushConst(1, 0);
    }
}

// ---------------- 运算 ----------------
void CodeGen::emitBoolNot() {
    ins(kNot(VmNum::U8));        // 直接发射 NOT_U8，避免递归
    pushConst(1, 1);
    ins(kAnd(VmNum::U8));
}

void CodeGen::emitBinaryOp(BinaryOp op, Type* t, FuncInstance* inst) {
    VmNum n = vmNum(t);
    switch (op) {
        case BinaryOp::Add: ins(kAdd(n)); break;
        case BinaryOp::Sub: ins(kSub(n)); break;
        case BinaryOp::Mul: ins(opMul(n)); break;
        case BinaryOp::Div: ins(opDiv(n)); break;
        case BinaryOp::Mod: {
            // a % b = a - (a/b)*b
            int w = vmW(n);
            popReg(0, w);
            storeSlot(0, w, tempOffset(inst, 0));   // b
            popReg(1, w);
            storeSlot(1, w, tempOffset(inst, 1));   // a
            loadSlot(0, w, tempOffset(inst, 1)); pushReg(0, w);
            loadSlot(0, w, tempOffset(inst, 0)); pushReg(0, w);
            ins(opDiv(n));
            loadSlot(0, w, tempOffset(inst, 0)); pushReg(0, w);
            ins(opMul(n));
            popReg(0, w);
            storeSlot(0, w, tempOffset(inst, 2));   // q*b -> temp2
            loadSlot(0, w, tempOffset(inst, 1)); pushReg(0, w);  // a
            loadSlot(0, w, tempOffset(inst, 2)); pushReg(0, w);  // q*b
            ins(kSub(n));
            em_.popD(w);
            break;
        }
        case BinaryOp::Shl: ins(kShl(n)); break;
        case BinaryOp::Shr: ins(t->isSigned() ? kShri(n) : kShr(n)); break;
        case BinaryOp::And: ins(kAnd(n)); break;
        case BinaryOp::Or: ins(kOr(n)); break;
        case BinaryOp::Xor: {
            int w = vmW(n);
            popReg(0, w);
            storeSlot(0, w, tempOffset(inst, 0));   // b
            popReg(1, w);
            storeSlot(1, w, tempOffset(inst, 1));   // a
            loadSlot(0, w, tempOffset(inst, 1)); pushReg(0, w);
            loadSlot(0, w, tempOffset(inst, 0)); pushReg(0, w);
            ins(kNot(n));
            ins(kAnd(n));
            loadSlot(0, w, tempOffset(inst, 1)); pushReg(0, w);
            ins(kNot(n));
            loadSlot(0, w, tempOffset(inst, 0)); pushReg(0, w);
            ins(kAnd(n));
            ins(kOr(n));
            em_.popD(w);
            break;
        }
        default: break;
    }
    em_.popD(vmW(n) * 2);
    em_.pushD(vmW(n));
}

void CodeGen::emitCompare(BinaryOp op, Type* t, FuncInstance* inst) {
    VmNum n = vmNum(t);
    int w = vmW(n);
    switch (op) {
        case BinaryOp::Lt: ins(opCmpLT(n)); break;
        case BinaryOp::Gt: ins(opCmpGT(n)); break;
        case BinaryOp::Le: ins(opCmpGT(n)); emitBoolNot(); break;
        case BinaryOp::Ge: ins(opCmpLT(n)); emitBoolNot(); break;
        case BinaryOp::Eq: ins(opCmpEQ(n)); break;
        case BinaryOp::Ne: ins(opCmpEQ(n)); emitBoolNot(); break;
        default: break;
    }
    em_.popD(w * 2);
    em_.pushD(1);
}

// ---------------- 类型转换（栈上） ----------------
void CodeGen::coerceOnStack(Type* from, Type* to, FuncInstance* inst) {
    if (!from || !to) return;
    if (from->equals(to)) return;
    if (from->kind == TypeKind::Err || to->kind == TypeKind::Err) return;
    if (from->isStruct() && to->isStruct()) return;
    if (from->kind == TypeKind::Exist && to->kind == TypeKind::Exist) return;

    // 具体类型 → 存在类型（boxing：压 vt）
    if (to->kind == TypeKind::Exist) {
        const std::string* typeName = nullptr;
        if (from->isStruct()) typeName = &from->name;
        else if (from->kind == TypeKind::Ptr && from->pointee && from->pointee->isStruct())
            typeName = &from->pointee->name;
        if (typeName) {
            popReg(1, 8);                 // data 地址
            pushReg(1, 8);                // 先留 data
            pushReg(kBcBase, 8);
            auto wi = sema_->witnessIndex.find(to->name + "::" + *typeName);
            int rel = (wi != sema_->witnessIndex.end()) ? sema_->witnesses[wi->second].dataOff : 0;
            uint64_t abs = static_cast<uint64_t>(dataStart_) + static_cast<uint64_t>(rel);
            ins(kRegMovi(VmNum::U64), "R1, " + std::to_string(abs));
            pushReg(1, 8);
            ins(OP_ADD_PTR);
            return;
        }
        return;
    }

    // bool/char → u8
    if (from->kind == TypeKind::Bool || from->kind == TypeKind::Char)
        from = Type::make(TypeKind::U8);
    if (to->kind == TypeKind::Bool || to->kind == TypeKind::Char) {
        if (from->kind == TypeKind::U8) return;
        // 数值 → bool：规范化 != 0
        int w = typeWidth(from);
        if (w <= 0) w = 8;
        pushConst(w, 0);
        ins(opCmpEQ(vmNum(from)));
        emitBoolNot();
        return;
    }

    // 整数 → 指针：零扩展到 8 字节（0 为 null，或显式地址转换）
    if (to->kind == TypeKind::Ptr && (from->isNumeric() || from->kind == TypeKind::Ent)) {
        int wf = from->kind == TypeKind::Ent ? 4 : typeWidth(from);
        if (wf <= 0) wf = 8;
        if (wf < 8) {
            popReg(0, wf);
            pushReg(0, 8);
        }
        return;
    }
    if (from->isNumeric() || from->kind == TypeKind::Ent) {
        if (to->isNumeric() || to->kind == TypeKind::Ent) {
            int wf = from->kind == TypeKind::Ent ? 4 : typeWidth(from);
            int wt = to->kind == TypeKind::Ent ? 4 : typeWidth(to);
            bool ff = from->isFloat();
            bool tf = to->isFloat();
            if (!ff && !tf) {
                if (wf == wt) return;
                if (wf < wt) {
                    popReg(0, wf);
                    if (from->isSigned()) {
                        int shift = (wt - wf) * 8;
                        pushReg(0, wt);
                        pushConst(wt, static_cast<uint64_t>(shift));
                        ins(kShl(vmNum(to)));
                        popReg(0, wt);
                        pushReg(0, wt);
                        pushConst(wt, static_cast<uint64_t>(shift));
                        ins(kShri(vmNum(to)));
                    } else {
                        pushReg(0, wt);   // 零扩展
                    }
                } else {
                    popReg(0, wf);
                    pushReg(0, wt);       // 截断
                }
                return;
            }
            if (ff && !tf) {
                if (wt == 4) {
                    ins(to->isSigned() ? OP_CVT_F64_I32 : OP_CVT_F64_U32);
                } else {
                    callHelper(to->isSigned() ? "__i64_from_f64" : "__u64_from_f64", 8, inst);
                }
                return;
            }
            if (!ff && tf) {
                int w = wf;
                if (w < 4) {
                    coerceOnStack(from, Type::make(from->isSigned() ? TypeKind::I32 : TypeKind::U32), inst);
                    w = 4;
                }
                if (w == 4) {
                    ins(from->isSigned() ? OP_CVT_I32_F64 : OP_CVT_U32_F64);
                } else {
                    callHelper(from->isSigned() ? "__f64_from_i64" : "__f64_from_u64", 8, inst);
                }
                return;
            }
            return;
        }
    }
    // 指针 ↔ u64/ent/指针
    if (from->kind == TypeKind::Ptr && (to->kind == TypeKind::U64 || to->kind == TypeKind::Ptr ||
                                        to->kind == TypeKind::Ent)) return;
    if ((from->kind == TypeKind::U64 || from->kind == TypeKind::Ent) && to->kind == TypeKind::Ptr) return;
    if (from->kind == TypeKind::Ent && to->kind == TypeKind::U32) return;
    if (from->kind == TypeKind::U32 && to->kind == TypeKind::Ent) return;
    // 其他：忽略（sema 已检查）
}

// ---------------- 赋值 / 复合赋值 / ++-- ----------------
void CodeGen::emitAssign(Expr* e, FuncInstance* inst) {
    Type* t = e->type;
    emitAddress(e->lhs.get(), inst);       // [addr]
    emitExpr(e->rhs.get(), inst);          // [addr][value]
    coerceOnStack(e->rhs->type, t, inst);
    if (t->isStruct()) {
        // [dst][src] → MEMCPY
        popReg(0, 8);
        storeSlot(0, 8, tempOffset(inst, 0));   // src
        popReg(1, 8);
        storeSlot(1, 8, tempOffset(inst, 1));   // dst
        ins(OP_MEMCPY, "1 " + std::to_string(tempOffset(inst, 1)) +
                       " 1 " + std::to_string(tempOffset(inst, 0)) +
                       " " + std::to_string(sema_->sizeOf(t)));
        pushReg(1, 8);                          // 留 dst
        return;
    }
    if (t->kind == TypeKind::Exist) {
        popReg(0, 8);
        storeSlot(0, 8, tempOffset(inst, 0));   // vt
        popReg(1, 8);
        storeSlot(1, 8, tempOffset(inst, 1));   // data
        popReg(2, 8);
        storeSlot(2, 8, tempOffset(inst, 2));   // box addr
        storePtr(1, 8, tempOffset(inst, 2));    // *(addr) = data
        loadSlot(0, 8, tempOffset(inst, 2));
        pushReg(0, 8);
        pushConst(8, 8);
        ins(OP_ADD_PTR);
        popReg(0, 8);
        storeSlot(0, 8, tempOffset(inst, 3));
        storePtr(0, 8, tempOffset(inst, 3));    // *(addr+8) = vt
        pushReg(2, 8);                          // 留 addr
        return;
    }
    int w = typeWidth(t);
    if (w <= 0) w = 8;
    popReg(0, w);                               // value
    popReg(1, 8);                               // addr
    storeSlot(1, 8, tempOffset(inst, 0));
    storePtr(0, w, tempOffset(inst, 0));
    pushReg(0, w);                              // 留值
}

void CodeGen::emitCompoundAssign(Expr* e, FuncInstance* inst) {
    Type* t = e->type;
    BinaryOp op;
    switch (e->asOp) {
        case AssignOp::Add: op = BinaryOp::Add; break;
        case AssignOp::Sub: op = BinaryOp::Sub; break;
        case AssignOp::Mul: op = BinaryOp::Mul; break;
        case AssignOp::Div: op = BinaryOp::Div; break;
        case AssignOp::Mod: op = BinaryOp::Mod; break;
        case AssignOp::Shl: op = BinaryOp::Shl; break;
        case AssignOp::Shr: op = BinaryOp::Shr; break;
        case AssignOp::And: op = BinaryOp::And; break;
        case AssignOp::Or: op = BinaryOp::Or; break;
        case AssignOp::Xor: op = BinaryOp::Xor; break;
        default: op = BinaryOp::Add; break;
    }
    if (t->kind == TypeKind::Ptr) {
        // 指针 += 偏移
        emitAddress(e->lhs.get(), inst);
        emitExpr(e->rhs.get(), inst);
        coerceOnStack(e->rhs->type, Type::make(TypeKind::U64), inst);
        ins(OP_ADD_PTR);
        popReg(0, 8);
        popReg(1, 8);
        storeSlot(1, 8, tempOffset(inst, 0));
        storePtr(0, 8, tempOffset(inst, 0));
        pushReg(0, 8);
        return;
    }
    emitAddress(e->lhs.get(), inst);       // [addr]
    emitExpr(e->lhs.get(), inst);          // [addr][old]
    coerceOnStack(e->lhs->type, t, inst);
    emitExpr(e->rhs.get(), inst);          // [addr][old][v]
    coerceOnStack(e->rhs->type, t, inst);
    emitBinaryOp(op, t, inst);             // [addr][new]
    int w = typeWidth(t);
    if (w <= 0) w = 8;
    popReg(0, w);
    popReg(1, 8);
    storeSlot(1, 8, tempOffset(inst, 0));
    storePtr(0, w, tempOffset(inst, 0));
    pushReg(0, w);
}

void CodeGen::emitIncDec(Expr* e, FuncInstance* inst) {
    Type* t = e->type;
    bool inc = (e->unOp == UnaryOp::PreInc || e->unOp == UnaryOp::PostInc);
    bool pre = !e->postfix;
    emitAddress(e->lhs.get(), inst);
    popReg(1, 8);
    storeSlot(1, 8, tempOffset(inst, 0));   // addr
    if (t->kind == TypeKind::Ptr) {
        loadPtr(0, 8, tempOffset(inst, 0));
        pushReg(0, 8);
        pushConst(8, 1);
        ins(inc ? OP_ADD_PTR : OP_SUB_PTR);
        popReg(1, 8);
        storePtr(1, 8, tempOffset(inst, 0));
        pushReg(1, 8);
        return;
    }
    int w = typeWidth(t);
    if (w <= 0) w = 8;
    loadPtr(0, w, tempOffset(inst, 0));     // R0 = old
    pushReg(0, w);                          // [old]
    pushConst(w, 1);
    ins(inc ? kAdd(vmNum(t)) : kSub(vmNum(t)));   // [new]
    popReg(1, w);
    storePtr(1, w, tempOffset(inst, 0));    // *addr = new
    if (pre) {
        pushReg(1, w);
    } else {
        pushReg(0, w);                      // R0 仍是 old
    }
}

// ---------------- 调用 ----------------
int CodeGen::instanceIndex(FuncInstance* fi) {
    auto it = std::find(sema_->instances.begin(), sema_->instances.end(), fi);
    return it == sema_->instances.end() ? -1 : static_cast<int>(it - sema_->instances.begin());
}

void CodeGen::callHelper(const std::string& name, int argWidth, FuncInstance* inst) {
    FuncInstance* h = helper(name);
    if (!h) { pushConst(8, 0); return; }
    int idx = instanceIndex(h);
    int off = em_.counting ? 0 : codeOff_[idx];
    // 参数已在栈上（caller 的 sp）：先取出，SCOPE_PUSH 后再放回帧内
    popReg(0, argWidth);
    ins(OP_SCOPE_PUSH, "", "call helper " + name);
    pushReg(0, argWidth);
    int retOff = em_.pc + 31 + (em_.counting ? 0 : codeOff_[curFuncIdx_]);
    ins(kRegPush(VmNum::U64), "R12");
    ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(retOff));
    pushReg(0, 8);
    em_.popD(8);   // 调用机械部分记账归零
    ins(OP_ADD_U64);
    ins(kRegPush(VmNum::U64), "R12");
    ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(off));
    pushReg(0, 8);
    em_.popD(8);
    ins(OP_ADD_U64);
    ins(OP_JMP_IND);
    ins(OP_STACK_PTR_MOVE, std::to_string(argWidth), "pop helper args");
    em_.popD(argWidth);
    ins(OP_SCOPE_POP);   // sp 恢复到调用前位置
}

void CodeGen::emitStaticCall(FuncInstance* callee, std::vector<ExprPtr>& args, FuncInstance* inst) {
    int idx = instanceIndex(callee);
    int calleeOff = em_.counting ? 0 : codeOff_[idx];
    if (args.size() > 8) {
        Diag::error({0, 0}, "internal: too many arguments (>8)");
        return;
    }
    // 参数在 SCOPE_PUSH 前求值（用调用者的 base），存入 R4+i
    for (size_t i = 0; i < args.size(); ++i) {
        emitExpr(args[i].get(), inst);
        if (i < callee->paramTypes.size()) {
            coerceOnStack(args[i]->type, callee->paramTypes[i], inst);
        }
        Type* pt = callee->paramTypes[i];
        int w = pt->isStruct() ? 8 : (pt->kind == TypeKind::Exist ? 16 : typeWidth(pt));
        if (w <= 0) w = 8;
        popReg(4 + static_cast<int>(i), w);
    }
    ins(OP_SCOPE_PUSH, "", "call " + callee->key);
    if (callee->retBytes > 0) {
        ins(OP_NEW_STACK, std::to_string(callee->retBytes), "ret area");
        em_.pushD(callee->retBytes);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        Type* pt = callee->paramTypes[i];
        int w = pt->isStruct() ? 8 : (pt->kind == TypeKind::Exist ? 16 : typeWidth(pt));
        if (w <= 0) w = 8;
        pushReg(4 + static_cast<int>(i), w);
    }
    int retOff = em_.pc + 31 + (em_.counting ? 0 : codeOff_[curFuncIdx_]);
    ins(kRegPush(VmNum::U64), "R12");
    ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(retOff));
    pushReg(0, 8);
    em_.popD(8);   // 调用机械部分记账归零（真实 sp 由 SCOPE_POP/NEW_STACK 统一管理）
    ins(OP_ADD_U64);
    ins(kRegPush(VmNum::U64), "R12");
    ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(calleeOff));
    pushReg(0, 8);
    em_.popD(8);
    ins(OP_ADD_U64);
    ins(OP_JMP_IND);
    // 读返回值到寄存器（SCOPE_POP 会重置 sp，不能把值留在栈上）
    if (callee->retBytes > 0) {
        if (callee->retType->isStruct()) {
            loadSlot(0, 8, 0);
        } else if (callee->retType->kind == TypeKind::Exist) {
            loadSlot(1, 8, 0);   // data
            loadSlot(0, 8, 8);   // vt
        } else {
            int w = typeWidth(callee->retType);
            if (w <= 0) w = 8;
            loadSlot(0, w, 0);
        }
    }
    ins(OP_STACK_PTR_MOVE, std::to_string(callee->retBytes + callee->paramBytes), "pop call frame");
    em_.popD(callee->retBytes + callee->paramBytes);
    ins(OP_SCOPE_POP);   // sp 恢复到调用作用域基址 = 调用前 sp
    // 压回返回值
    if (callee->retBytes > 0) {
        if (callee->retType->isStruct()) {
            pushReg(0, 8);
        } else if (callee->retType->kind == TypeKind::Exist) {
            pushReg(1, 8);       // data
            pushReg(0, 8);       // vt（在栈顶）
        } else {
            int w = typeWidth(callee->retType);
            if (w <= 0) w = 8;
            pushReg(0, w);
        }
    }
}

// 对象地址（方法调用的 this）
void CodeGen::emitObjectAddress(Expr* obj, FuncInstance* inst) {
    if (!obj) { pushConst(8, 0); return; }
    if (obj->type && obj->type->kind == TypeKind::Ptr) {
        emitExpr(obj, inst);
    } else {
        emitAddress(obj, inst);
    }
}

// 静态方法调用（obj.method(args)）
void CodeGen::emitMethodCall(FuncInstance* callee, Expr* obj, std::vector<ExprPtr>& args, FuncInstance* inst) {
    int idx = instanceIndex(callee);
    int calleeOff = em_.counting ? 0 : codeOff_[idx];
    if (args.size() + 1 > 8) {
        Diag::error({0, 0}, "internal: too many arguments (>7)");
        return;
    }
    // this + 参数在 SCOPE_PUSH 前求值（调用者 base）
    emitObjectAddress(obj, inst);
    popReg(4, 8);
    for (size_t i = 0; i < args.size(); ++i) {
        emitExpr(args[i].get(), inst);
        if (i + 1 < callee->paramTypes.size()) {
            coerceOnStack(args[i]->type, callee->paramTypes[i + 1], inst);
        }
        Type* pt = callee->paramTypes[i + 1];
        int w = pt->isStruct() ? 8 : (pt->kind == TypeKind::Exist ? 16 : typeWidth(pt));
        if (w <= 0) w = 8;
        popReg(5 + static_cast<int>(i), w);
    }
    ins(OP_SCOPE_PUSH, "", "method " + callee->key);
    if (callee->retBytes > 0) {
        ins(OP_NEW_STACK, std::to_string(callee->retBytes), "ret area");
        em_.pushD(callee->retBytes);
    }
    pushReg(4, 8);
    for (size_t i = 0; i < args.size(); ++i) {
        Type* pt = callee->paramTypes[i + 1];
        int w = pt->isStruct() ? 8 : (pt->kind == TypeKind::Exist ? 16 : typeWidth(pt));
        if (w <= 0) w = 8;
        pushReg(5 + static_cast<int>(i), w);
    }
    int retOff = em_.pc + 31 + (em_.counting ? 0 : codeOff_[curFuncIdx_]);
    ins(kRegPush(VmNum::U64), "R12");
    ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(retOff));
    pushReg(0, 8);
    em_.popD(8);   // 调用机械部分记账归零
    ins(OP_ADD_U64);
    ins(kRegPush(VmNum::U64), "R12");
    ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(calleeOff));
    pushReg(0, 8);
    em_.popD(8);
    ins(OP_ADD_U64);
    ins(OP_JMP_IND);
    // 读返回值到寄存器（SCOPE_POP 会重置 sp）
    if (callee->retBytes > 0) {
        if (callee->retType->isStruct()) {
            loadSlot(0, 8, 0);
        } else if (callee->retType->kind == TypeKind::Exist) {
            loadSlot(1, 8, 0);   // data
            loadSlot(0, 8, 8);   // vt
        } else {
            int w = typeWidth(callee->retType);
            if (w <= 0) w = 8;
            loadSlot(0, w, 0);
        }
    }
    ins(OP_STACK_PTR_MOVE, std::to_string(callee->retBytes + callee->paramBytes), "pop method frame");
    em_.popD(callee->retBytes + callee->paramBytes);
    ins(OP_SCOPE_POP);   // sp 恢复到调用前位置
    // 压回返回值
    if (callee->retBytes > 0) {
        if (callee->retType->isStruct()) {
            pushReg(0, 8);
        } else if (callee->retType->kind == TypeKind::Exist) {
            pushReg(1, 8);       // data
            pushReg(0, 8);       // vt（在栈顶）
        } else {
            int w = typeWidth(callee->retType);
            if (w <= 0) w = 8;
            pushReg(0, w);
        }
    }
}

// 动态派发：存在类型方法调用
void CodeGen::emitDynamicCall(Expr* e, FuncInstance* inst) {
    // lhs 是盒：data 槽 / vt 槽
    Expr* obj = e->lhs.get();
    int dataOff = 0, vtOff = 8;
    VarSlot* vs = nullptr;
    if (obj->kind == ExprKind::VarRef) {
        vs = findSlot(inst, obj->name);
    }
    int mIdx = e->resolved ? e->resolved->witnessMethodIndex : 0;
    // 协议方法参数类型（用于参数强制转换）
    std::vector<Type*> protoParamTypes;
    {
        auto pit = sema_->protocols.find(e->resolved ? e->resolved->protoName : "");
        if (pit != sema_->protocols.end()) {
            for (auto& m : pit->second->methods) {
                if (m.index == mIdx) {
                    for (size_t i = 1; i < m.params.size(); ++i) {
                        protoParamTypes.push_back(sema_->resolveType(m.params[i].typeName));
                    }
                    break;
                }
            }
        }
    }
    Type* rt = e->type;
    int retBytes = 0;
    if (rt && rt->isStruct()) retBytes = 8;
    else if (rt && rt->kind == TypeKind::Exist) retBytes = 16;
    else if (rt) { retBytes = typeWidth(rt); if (retBytes < 0) retBytes = 0; }

    if (vs) {
        // === 调用者作用域内（SCOPE_PUSH 之前）：取盒 + 求值参数 ===
        // this（data 指针）→ R3；vt → temp0；方法偏移 → R2
        loadSlot(3, 8, vs->offset + dataOff);
        loadSlot(1, 8, vs->offset + vtOff);
        storeSlot(1, 8, tempOffset(inst, 0));       // temp0 = vt
        loadSlot(0, 8, tempOffset(inst, 0));
        pushReg(0, 8);
        pushConst(8, static_cast<uint64_t>(mIdx) * 4);
        ins(OP_ADD_PTR);
        popReg(0, 8);
        storeSlot(0, 8, tempOffset(inst, 1));       // temp1 = vt+mIdx*4
        loadPtr(2, 4, tempOffset(inst, 1));         // R2 = 方法偏移
        // 方法参数 → R5+i（与静态调用一致：压栈前求值，用调用者的 base）
        if (e->args.size() > 7) {
            Diag::error({0, 0}, "internal: too many dynamic arguments (>7)");
        }
        for (size_t i = 0; i < e->args.size(); ++i) {
            emitExpr(e->args[i].get(), inst);
            if (i < protoParamTypes.size()) {
                coerceOnStack(e->args[i]->type, protoParamTypes[i], inst);
            }
            Type* at = (i < protoParamTypes.size()) ? protoParamTypes[i] : e->args[i]->type;
            int w = at->isStruct() ? 8 : (at->kind == TypeKind::Exist ? 16 : typeWidth(at));
            if (w <= 0) w = 8;
            popReg(5 + static_cast<int>(i), w);
        }
        // === 新作用域：压 this + 参数 + 调用 ===
        ins(OP_SCOPE_PUSH, "", "dynamic dispatch " + std::string(e->name));
        if (retBytes > 0) {
            ins(OP_NEW_STACK, std::to_string(retBytes), "ret area");
            em_.pushD(retBytes);
        }
        pushReg(3, 8);   // this（data 指针）
        for (size_t i = 0; i < e->args.size(); ++i) {
            Type* at = (i < protoParamTypes.size()) ? protoParamTypes[i] : e->args[i]->type;
            int w = at->isStruct() ? 8 : (at->kind == TypeKind::Exist ? 16 : typeWidth(at));
            if (w <= 0) w = 8;
            pushReg(5 + static_cast<int>(i), w);
        }
        // retaddr（后续指令：PUSH(2)+MOVI(10)+PUSH(2)+ADD(1)+PUSH(2)+PUSH(2)+ADD(1)+JMP_IND(1)=21 字节）
        int retOff = em_.pc + 21 + (em_.counting ? 0 : codeOff_[curFuncIdx_]);
        ins(kRegPush(VmNum::U64), "R12");
        ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(retOff));
        pushReg(0, 8);
        em_.popD(8);   // 调用机械部分记账归零
        ins(OP_ADD_U64);
        // target = bcBase + off
        ins(kRegPush(VmNum::U64), "R12");
        pushReg(2, 8);
        em_.popD(8);
        ins(OP_ADD_U64);
        ins(OP_JMP_IND);
        // 返回值读取到寄存器（SCOPE_POP 会重置 sp）
        if (rt && rt->isStruct()) {
            loadSlot(0, 8, 0);
        } else if (rt && rt->kind == TypeKind::Exist) {
            loadSlot(1, 8, 0);   // data
            loadSlot(0, 8, 8);   // vt
        } else if (rt) {
            int w = typeWidth(rt);
            if (w <= 0) w = 8;
            loadSlot(0, w, 0);
        }
        // 弹出 ret 区 + 参数 + this（参数按协议参数类型宽度）
        int argBytes = 8;
        for (size_t i = 0; i < e->args.size(); ++i) {
            Type* at = (i < protoParamTypes.size()) ? protoParamTypes[i] : e->args[i]->type;
            argBytes += at->isStruct() ? 8 : (at->kind == TypeKind::Exist ? 16 : typeWidth(at));
        }
        ins(OP_STACK_PTR_MOVE, std::to_string(retBytes + argBytes), "pop method args");
        em_.popD(retBytes + argBytes);
        ins(OP_SCOPE_POP);   // sp 恢复到调用前位置
        // 压回返回值
        if (rt && rt->isStruct()) {
            pushReg(0, 8);
        } else if (rt && rt->kind == TypeKind::Exist) {
            pushReg(1, 8);       // data
            pushReg(0, 8);       // vt（在栈顶）
        } else if (rt) {
            int w = typeWidth(rt);
            if (w <= 0) w = 8;
            pushReg(0, w);
        }
    } else {
        pushConst(8, 0);
    }
}

void CodeGen::emitExternCall(Expr* e, FuncInstance* inst) {
    ResolvedCall* rc = e->resolved;
    int idx = rc ? rc->externIndex : 0;
    Decl* d = sema_->externs[idx];
    int argBytes = 0;
    std::vector<Type*> argTypes;
    for (auto& tn : d->externTypes) argTypes.push_back(sema_->resolveType(tn));
    int retBytes = 0;
    Type* rt = sema_->resolveType(d->externRetType);
    retBytes = typeWidth(rt);
    if (retBytes < 0) retBytes = 0;
    if (retBytes > 8) retBytes = 8;

    if (e->args.size() > 8) {
        Diag::error({0, 0}, "internal: too many extern arguments (>8)");
        return;
    }
    // 调用者作用域内求值参数 → R4+i（SCOPE_PUSH 后基址已变）
    for (size_t i = 0; i < e->args.size() && i < argTypes.size(); ++i) {
        emitExpr(e->args[i].get(), inst);
        coerceOnStack(e->args[i]->type, argTypes[i], inst);
        int w = typeWidth(argTypes[i]);
        if (w <= 0) w = 8;
        argBytes += w;
        popReg(4 + static_cast<int>(i), w);
    }
    ins(OP_SCOPE_PUSH, "", "extern " + d->name);
    for (size_t i = 0; i < e->args.size() && i < argTypes.size(); ++i) {
        int w = typeWidth(argTypes[i]);
        if (w <= 0) w = 8;
        pushReg(4 + static_cast<int>(i), w);
    }
    if (retBytes > 0) {
        ins(OP_NEW_STACK, std::to_string(retBytes), "extern ret area");
        em_.pushD(retBytes);
    }
    ins(OP_EXTERN_CALL, std::to_string(idx) + " 0 " + std::to_string(argBytes));
    // 返回值取到寄存器（SCOPE_POP 会重置 sp）
    if (retBytes > 0) {
        loadSlot(0, retBytes, argBytes);
    }
    ins(OP_STACK_PTR_MOVE, std::to_string(argBytes + retBytes), "pop extern frame");
    em_.popD(argBytes + retBytes);
    ins(OP_SCOPE_POP);   // sp 恢复到调用前位置
    if (retBytes > 0) {
        pushReg(0, retBytes);
    }
}

void CodeGen::emitCallExpr(Expr* e, FuncInstance* inst) {
    ResolvedCall* rc = e->resolved;
    const std::string& name = e->callee;
    // ECS 无模板调用
    if (name == "spawn" || name == "despawn" || name == "alive" || name == "entity_count") {
        emitEcs(e, inst, name, "");
        return;
    }
    // 内建
    if (name == "print") {
        for (auto& a : e->args) emitPrint(a.get(), inst);
        return;
    }
    if (name == "printc") {
        emitExpr(e->args[0].get(), inst);
        coerceOnStack(e->args[0]->type, Type::make(TypeKind::U8), inst);
        ins(OP_COUT_CHAR8, "", "printc");
        em_.popD(1);
        return;
    }
    if (name == "sqrt" || name == "log") {
        Type* at = e->args[0]->type;
        emitExpr(e->args[0].get(), inst);
        coerceOnStack(at, Type::make(TypeKind::F64), inst);
        ins(name == "sqrt" ? OP_SQRT_F64 : OP_LOG_F64);
        return;
    }
    if (name == "getsystem") {
        // GET_SYSTEM：压入 u64（低16位=平台，次16位=架构）
        ins(OP_GET_SYSTEM, "", "getsystem");
        em_.pushD(8);
        return;
    }
    if (name == "dl_reg") {
        // dl_reg(fnPtr, sigStr) → u32：参数先在调用者作用域求值到 R4/R5，
        // 再 SCOPE_PUSH 压栈（fn→base+0, sig→base+8），DL_REG(0,8)
        emitExpr(e->args[0].get(), inst);
        coerceOnStack(e->args[0]->type, Type::make(TypeKind::U64), inst);
        popReg(4, 8);
        emitExpr(e->args[1].get(), inst);
        coerceOnStack(e->args[1]->type, Type::make(TypeKind::U64), inst);
        popReg(5, 8);
        ins(OP_SCOPE_PUSH, "", "dl_reg");
        pushReg(4, 8);
        pushReg(5, 8);
        ins(OP_DL_REG, "0 8", "dl_reg");
        em_.pushD(4);                     // VM 压入 u32 索引
        popReg(0, 4);
        em_.popD(4);
        ins(OP_STACK_PTR_MOVE, "16");
        em_.popD(16);
        ins(OP_SCOPE_POP);
        pushReg(0, 4);                    // 结果 u32
        em_.pushD(4);
        return;
    }
    if (name == "dl_call") {
        // dl_call(idx, sigLiteral, args...)：签名串决定参数/返回值布局（编译期解析）。
        // 参数先在调用者作用域求值到 R4..，再 SCOPE_PUSH 压栈（idx→base+0，值参数→base+8..）
        std::string retName;
        std::vector<std::string> argNames;
        if (e->args.size() < 2 || e->args[1]->kind != ExprKind::StrLit ||
            !dlParseSig(e->args[1]->strVal, retName, argNames)) {
            pushConst(8, 0);
            return;
        }
        Type* retT = dlTypeByName(retName);
        int retBytes = retT ? typeWidth(retT) : 0;
        if (retBytes < 0) retBytes = 0;
        if (retBytes > 8) retBytes = 8;
        emitExpr(e->args[0].get(), inst);                       // idx → R4
        coerceOnStack(e->args[0]->type, Type::make(TypeKind::U32), inst);
        popReg(4, 4);
        size_t n = std::min<size_t>(e->args.size() - 2, argNames.size());
        int argBytes = 0;
        for (size_t i = 0; i < n; ++i) {
            Type* at = dlTypeByName(argNames[i]);
            int w = typeWidth(at ? at : Type::make(TypeKind::U64));
            emitExpr(e->args[i + 2].get(), inst);
            coerceOnStack(e->args[i + 2]->type, at ? at : Type::make(TypeKind::U64), inst);
            popReg(5 + static_cast<int>(i), w);
            argBytes += w;
        }
        ins(OP_SCOPE_PUSH, "", "dl_call");
        pushReg(4, 4);                                          // idx → base+0
        for (size_t i = 0; i < n; ++i) {
            Type* at = dlTypeByName(argNames[i]);
            int w = typeWidth(at ? at : Type::make(TypeKind::U64));
            pushReg(5 + static_cast<int>(i), w);
        }
        if (retBytes > 0) { ins(OP_NEW_STACK, std::to_string(retBytes), "dl ret"); em_.pushD(retBytes); }
        // 布局：idx(4B, base+0) → 参数(base+4..) → 返回值(base+4+argBytes)
        ins(OP_DL_CALL, "0 4 " + std::to_string(4 + argBytes));
        if (retBytes > 0) loadSlot(0, retBytes, 4 + argBytes);
        ins(OP_STACK_PTR_MOVE, std::to_string(8 + argBytes + retBytes), "pop dl frame");
        em_.popD(8 + argBytes + retBytes);
        ins(OP_SCOPE_POP);
        if (retBytes > 0) pushReg(0, retBytes);
        return;
    }
    if (name == "dload") {
        // dload("x.so") → dlopen("lib/x.so", RTLD_NOW)：动态链接默认根目录 = lib/
        int idx = autoExternIndex(sema_, "dlopen");
        if (idx < 0) { pushConst(8, 0); return; }
        emitExpr(e->args[0].get(), inst);   // "lib/x.so" 地址（sema 已拼接前缀）→ R4
        popReg(4, 8);
        pushConst(4, 2);                    // RTLD_NOW → R5
        popReg(5, 4);
        ins(OP_SCOPE_PUSH, "", "dload");
        pushReg(4, 8);                      // base+0: 路径（8B）
        pushReg(5, 4);                      // base+8: 标志（4B）
        ins(OP_NEW_STACK, "8", "dlopen ret");
        em_.pushD(8);
        ins(OP_EXTERN_CALL, std::to_string(idx) + " 0 12"); // argBytes = 8+4
        loadSlot(0, 8, 12);
        ins(OP_STACK_PTR_MOVE, "20");
        em_.popD(20);
        ins(OP_SCOPE_POP);
        pushReg(0, 8);
        em_.pushD(8);
        return;
    }
    if (name == "memcpy") {
        // 走 extern memcpy
        externMemcpy(e, inst);
        return;
    }
    if (name == "free") {
        externFree(e, inst);
        return;
    }
    if (name == "bytecode_base") {
        pushReg(kBcBase, 8);
        return;
    }
    if (name == "manager_addr") {
        ins(OP_GET_ADDRS);
        ins(kRegPop(VmNum::U64), "R0", "mgr");
        ins(kRegPop(VmNum::U64), "R1");
        ins(kRegPop(VmNum::U64), "R2");
        ins(kRegPop(VmNum::U64), "R3");
        pushReg(0, 8);
        return;
    }
    if (name.rfind("atomic_", 0) == 0) {
        bool is64 = name.find("u64") != std::string::npos;
        VmNum n = is64 ? VmNum::U64 : VmNum::U32;
        int w = is64 ? 8 : 4;
        Type* wt = is64 ? Type::make(TypeKind::U64) : Type::make(TypeKind::U32);
        emitExpr(e->args[0].get(), inst);
        popReg(1, 8);
        storeSlot(1, 8, tempOffset(inst, 0));
        // 值参数必须 coerce 到目标宽度：字面量压 4 字节（u32），u64 指令 popReg(0,8)
        // 会多弹 4 字节导致栈错位（历史 bug：42 被写成 42<<32）
        if (name.find("atomic_load") == 0) {
            ins(is64 ? OP_ATOMIC_LOAD_U64 : OP_ATOMIC_LOAD_U32,
                "R0, 1, " + std::to_string(tempOffset(inst, 0)));
            pushReg(0, w);
        } else if (name.find("atomic_store") == 0) {
            emitExpr(e->args[1].get(), inst);
            coerceOnStack(e->args[1]->type, wt, inst);
            popReg(0, w);
            ins(is64 ? OP_ATOMIC_STORE_U64 : OP_ATOMIC_STORE_U32,
                "R0, 1, " + std::to_string(tempOffset(inst, 0)));
        } else if (name.find("atomic_xchg") == 0) {
            emitExpr(e->args[1].get(), inst);
            coerceOnStack(e->args[1]->type, wt, inst);
            popReg(0, w);
            ins(is64 ? OP_ATOMIC_XCHG_U64 : OP_ATOMIC_XCHG_U32,
                "R0, 1, " + std::to_string(tempOffset(inst, 0)));
            pushReg(0, w);
        } else if (name.find("atomic_add") == 0) {
            emitExpr(e->args[1].get(), inst);
            coerceOnStack(e->args[1]->type, wt, inst);
            popReg(0, w);
            ins(is64 ? OP_ATOMIC_ADD_U64 : OP_ATOMIC_ADD_U32,
                "R0, 1, " + std::to_string(tempOffset(inst, 0)));
            pushReg(0, w);
        } else if (name.find("atomic_cas") == 0) {
            // CAS(ptr, expected, desired)：desired → R3；expected 最后压栈
            // （VM 的 CAS 从栈顶弹 expected。注意 desired 不能放 R0：
            //  后续 expected 的 coerceOnStack 用 R0 中转会覆盖它）
            emitExpr(e->args[2].get(), inst);
            coerceOnStack(e->args[2]->type, wt, inst);
            popReg(3, w);
            emitExpr(e->args[1].get(), inst);
            coerceOnStack(e->args[1]->type, wt, inst);
            // 栈顶 = expected（w 字节）；指令弹 expected、压 U8 标志
            ins(is64 ? OP_ATOMIC_CAS_U64 : OP_ATOMIC_CAS_U32,
                "R3, 1, " + std::to_string(tempOffset(inst, 0)));
            em_.popD(w);
            em_.pushD(1);
            popReg(0, 1);      // 标志 → R0
            pushReg(0, 1);     // 结果 bool
        }
        return;
    }
    // 其余：extern 或普通函数
    if (rc && rc->isExtern) {
        emitExternCall(e, inst);
        return;
    }
    if (rc && rc->callee) {
        emitStaticCall(rc->callee, e->args, inst);
        return;
    }
    pushConst(8, 0);
}

// extern memcpy / free / malloc 辅助（编译器自动注册的 extern）
static int autoExternIndex(Sema* sema, const std::string& name) {
    for (size_t i = 0; i < sema->externs.size(); ++i) {
        if (sema->externs[i]->name == name) return static_cast<int>(i);
    }
    return -1;
}

void CodeGen::externMemcpy(Expr* e, FuncInstance* inst) {
    int idx = autoExternIndex(sema_, "memcpy");
    if (idx < 0) { pushConst(8, 0); return; }
    // 参数先在调用者作用域求值到 R4..R6，再 SCOPE_PUSH 后压栈：
    // 否则 mode0 加载会用新作用域基址读到错误槽位
    int n = static_cast<int>(std::min<size_t>(e->args.size(), 3));
    for (int i = 0; i < n; ++i) {
        emitExpr(e->args[i].get(), inst);
        coerceOnStack(e->args[i]->type, Type::make(TypeKind::U64), inst);
        popReg(4 + i, 8);
    }
    ins(OP_SCOPE_PUSH, "", "extern memcpy");
    for (int i = 0; i < n; ++i) pushReg(4 + i, 8);
    ins(OP_EXTERN_CALL, std::to_string(idx) + " 0 24");
    ins(OP_STACK_PTR_MOVE, "24");
    em_.popD(24);
    ins(OP_SCOPE_POP);
}

void CodeGen::externFree(Expr* e, FuncInstance* inst) {
    int idx = autoExternIndex(sema_, "free");
    if (idx < 0) { pushConst(8, 0); return; }
    // 同 externMemcpy：先求值参数到 R4，再 SCOPE_PUSH 压栈
    emitExpr(e->args[0].get(), inst);
    coerceOnStack(e->args[0]->type, Type::make(TypeKind::U64), inst);
    popReg(4, 8);
    ins(OP_SCOPE_PUSH, "", "extern free");
    pushReg(4, 8);
    ins(OP_EXTERN_CALL, std::to_string(idx) + " 0 8");
    ins(OP_STACK_PTR_MOVE, "8");
    em_.popD(8);
    ins(OP_SCOPE_POP);
}

void CodeGen::emitNew(Expr* e, FuncInstance* inst) {
    int idx = autoExternIndex(sema_, "malloc");
    if (idx < 0) { pushConst(8, 0); return; }
    int elemSize = sema_->sizeOf(sema_->resolveType(e->name));
    if (elemSize <= 0) elemSize = 1;
    ins(OP_SCOPE_PUSH, "", "new " + e->name + "[]");
    emitExpr(e->rhs.get(), inst);
    coerceOnStack(e->rhs->type, Type::make(TypeKind::U64), inst);
    if (elemSize != 1) {
        pushConst(8, static_cast<uint64_t>(elemSize));
        ins(OP_MUL_U64);
    }
    ins(OP_EXTERN_CALL, std::to_string(idx) + " 0 8");
    loadSlot(0, 8, 8);
    pushReg(0, 8);
    ins(OP_STACK_PTR_MOVE, "16");
    em_.popD(16);
    ins(OP_SCOPE_POP);
}

// ---------------- print ----------------
void CodeGen::emitPrint(Expr* a, FuncInstance* inst) {
    Type* t = a->type;
    emitExpr(a, inst);
    if (!t) return;
    switch (t->kind) {
        case TypeKind::Char:
            ins(OP_COUT_CHAR8, "", "print char");
            em_.popD(1);
            break;
        case TypeKind::Bool:
            // true/false 文本（helper：b ? "true" : "false"）
            callHelper("__print_bool", 1, inst);
            break;
        case TypeKind::U8:
            popReg(0, 1);
            pushReg(0, 4);
            ins(OP_COUT_CHAR32, "", "print u8");
            em_.popD(4);
            break;
        case TypeKind::U16:
            popReg(0, 2);
            pushReg(0, 4);
            ins(OP_COUT_CHAR32, "", "print u16");
            em_.popD(4);
            break;
        case TypeKind::U32: case TypeKind::Ent:
            ins(OP_COUT_CHAR32, "", "print u32");
            em_.popD(4);
            break;
        case TypeKind::U64:
            callHelper("__print_u64", 8, inst);
            break;
        case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            coerceOnStack(t, Type::make(TypeKind::I64), inst);
            callHelper("__print_i64", 8, inst);
            break;
        case TypeKind::F64:
            callHelper("__print_f64", 8, inst);
            break;
        case TypeKind::Ptr: {
            Type* p = t->pointee;
            if (p && (p->kind == TypeKind::U8 || p->kind == TypeKind::Char)) {
                callHelper("__print_str", 8, inst);
            } else {
                callHelper("__print_u64", 8, inst);
            }
            break;
        }
        case TypeKind::Struct:
            Diag::error(a->loc, "cannot print struct '" + t->name + "' directly");
            em_.popD(8);
            break;
        default:
            em_.popD(8);
            break;
    }
}

// ---------------- ECS ----------------
void CodeGen::worldAddr(uint64_t off) {
    pushReg(kWorld, 8);
    pushConst(8, off);
    ins(OP_ADD_PTR);
}

void CodeGen::emitEcs(Expr* e, FuncInstance* inst, const std::string& name, const std::string& tmpl) {
    auto t0 = tempOffset(inst, 0), t1 = tempOffset(inst, 1), t2 = tempOffset(inst, 2);
    auto t3 = tempOffset(inst, 3), t4 = tempOffset(inst, 4);

    if (name == "spawn") {
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(2, 4, t0);                    // R2 = count（R0 被 worldAddr 占用；branch 破坏 R0/R1）
        worldAddr(4);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(3, 4, t0);                    // R3 = cap
        // count >= cap → 错误
        pushReg(2, 4);
        pushReg(3, 4);
        ins(OP_CMP_LT_U32);                   // count < cap
        emitBoolNot();                 // count >= cap
        std::string lOk = newLabel("spawn_ok");
        std::string lFull = newLabel("spawn_full");
        branchOnFlag(lOk, lFull);
        label(lFull);
        // 打印错误并退出
        {
            // 字符串地址
            int rel = 0;
            for (auto& [s, r] : stringRelOff_) {
                if (s == "world full") { rel = r; break; }
            }
            pushReg(kBcBase, 8);
            ins(kRegMovi(VmNum::U64), "R1, " + std::to_string(static_cast<uint64_t>(dataStart_) + rel));
            pushReg(1, 8);
            ins(OP_ADD_PTR);
            callHelper("__print_str", 8, inst);
            ins(OP_END);
        }
        label(lOk);
        // *(u32*)world = count + 1（R2 = 旧 count 存活）
        pushReg(2, 4);                        // 旧 count（返回值）
        pushReg(2, 4);
        pushConst(4, 1);
        ins(OP_ADD_U32);
        popReg(2, 4);                         // R2 = count+1
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        storePtr(2, 4, t0);                   // *(world) = count+1
        // 栈上保留旧 count（ent 返回值）
        return;
    }
    if (name == "entity_count") {
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(0, 4, t0);
        pushReg(0, 4);
        return;
    }
    if (name == "alive") {
        emitExpr(e->args[0].get(), inst);
        popReg(3, 4);                          // e → R3（worldAddr 破坏 R0）
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(1, 4, t0);                     // R1 = count
        pushReg(3, 4);
        pushReg(1, 4);
        ins(OP_CMP_LT_U32);                    // e < count
        return;
    }
    if (name == "despawn") {
        emitExpr(e->args[0].get(), inst);
        popReg(3, 4);                          // e → R3
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(1, 4, t0);                     // R1 = count
        // if (e+1 == count) count = e
        pushReg(3, 4);
        pushConst(4, 1);
        ins(OP_ADD_U32);
        popReg(2, 4);                          // R2 = e+1
        pushReg(2, 4);
        pushReg(1, 4);
        ins(OP_CMP_EQ_U32);
        std::string lLast = newLabel("dsp_last");
        std::string lNot = newLabel("dsp_not");
        branchOnFlag(lLast, lNot);
        label(lLast);
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        storePtr(3, 4, t0);                    // count = e
        label(lNot);
        // 清所有组件的 bitset[e] = 0
        for (Decl* cd : sema_->components) {
            auto clIt = sema_->compLayout.find(cd->name);
            int bitsetOff = clIt != sema_->compLayout.end() ? clIt->second.bitsetOff : 0;
            worldAddr(static_cast<uint64_t>(bitsetOff));   // [addr]
            pushReg(3, 8);                     // [addr][e]
            ins(OP_ADD_PTR);                   // [addr+e]
            popReg(1, 8);
            storeSlot(1, 8, t0);
            ins(kRegMovi(VmNum::U8), "R2, 0");
            storePtr(2, 1, t0);
        }
        return;
    }
    // 组件相关（run 除外：tmpl 是系统名而非组件名，必须先放行到 run 分支）
    auto compIt = sema_->compLayout.find(tmpl);
    if (name != "run" && compIt == sema_->compLayout.end()) { pushConst(8, 0); return; }
    const Sema::CompLayout& cl = compIt->second;
    if (name == "has" || name == "remove") {
        emitExpr(e->args[0].get(), inst);
        popReg(3, 4);                          // e → R3
        worldAddr(static_cast<uint64_t>(cl.bitsetOff));   // [addr]
        pushReg(3, 8);                         // [addr][e]
        ins(OP_ADD_PTR);                       // [addr+e]
        popReg(1, 8);
        storeSlot(1, 8, t0);
        if (name == "has") {
            loadPtr(1, 1, t0);
            pushReg(1, 1);
        } else {
            ins(kRegMovi(VmNum::U8), "R2, 0");
            storePtr(2, 1, t0);
        }
        return;
    }
    if (name == "get") {
        emitExpr(e->args[0].get(), inst);
        popReg(3, 4);                          // e → R3
        worldAddr(static_cast<uint64_t>(cl.bitsetOff));   // [addr]
        pushReg(3, 8);                         // [addr][e]
        ins(OP_ADD_PTR);                       // [addr+e]
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(1, 1, t0);                     // presence
        pushReg(1, 1);
        std::string lHave = newLabel("get_have");
        std::string lNone = newLabel("get_none");
        branchOnFlag(lHave, lNone);
        label(lHave);
        worldAddr(static_cast<uint64_t>(cl.dataOff));     // [addr]
        pushReg(3, 8);                         // [addr][e]
        if (cl.size != 1) {
            pushConst(8, static_cast<uint64_t>(cl.size));
            ins(OP_MUL_U64);                   // [addr][e*size]
        }
        ins(OP_ADD_PTR);                       // [addr+e*size]
        std::string lEnd = newLabel("get_end");
        jumpTo(lEnd);
        label(lNone);
        pushConst(8, 0);
        label(lEnd);
        return;
    }
    if (name == "add") {
        emitExpr(e->args[0].get(), inst);
        popReg(3, 4);                          // e → R3
        // bitset[e] = 1
        worldAddr(static_cast<uint64_t>(cl.bitsetOff));   // [addr]
        pushReg(3, 8);                         // [addr][e]
        ins(OP_ADD_PTR);                       // [addr+e]
        popReg(1, 8);
        storeSlot(1, 8, t0);
        ins(kRegMovi(VmNum::U8), "R2, 1");
        storePtr(2, 1, t0);
        // base = world + dataOff + e*size → t1
        worldAddr(static_cast<uint64_t>(cl.dataOff));     // [addr]
        pushReg(3, 8);                         // [addr][e]
        if (cl.size != 1) {
            pushConst(8, static_cast<uint64_t>(cl.size));
            ins(OP_MUL_U64);                   // [addr][e*size]
        }
        ins(OP_ADD_PTR);                       // [addr+e*size]
        popReg(1, 8);
        storeSlot(1, 8, t1);
        // 逆序写入字段
        Decl* cd = sema_->structs[tmpl];
        if (cd) {
            for (int i = static_cast<int>(cd->fields.size()) - 1; i >= 0; --i) {
                size_t ai = static_cast<size_t>(i) + 1;
                if (ai >= e->args.size()) continue;
                Type* ft = sema_->resolveType(cd->fields[i].typeName);
                emitExpr(e->args[ai].get(), inst);
                coerceOnStack(e->args[ai]->type, ft, inst);
                int w = typeWidth(ft);
                if (w <= 0) w = 8;
                popReg(3, w);                  // value → R3（pushConst 会破坏 R0）
                loadSlot(1, 8, t1);
                pushReg(1, 8);
                if (cd->fields[i].offset != 0) {
                    pushConst(8, static_cast<uint64_t>(cd->fields[i].offset));
                    ins(OP_ADD_PTR);
                }
                popReg(1, 8);
                storeSlot(1, 8, t2);
                storePtr(3, w, t2);
            }
        }
        return;
    }
    if (name == "run") {
        // 系统迭代
        Decl* sys = sema_->systems[tmpl];
        if (!sys) { pushConst(8, 0); return; }
        FuncInstance* upd = sema_->getInstance("sys_" + tmpl + "_update", {});
        int updIdx = upd ? instanceIndex(upd) : -1;
        int updOff = (em_.counting || !upd) ? 0 : codeOff_[updIdx];
        // count → t3, e → t4
        worldAddr(0);
        popReg(1, 8);
        storeSlot(1, 8, t0);
        loadPtr(0, 4, t0);
        storeSlot(0, 4, t3);
        ins(kRegMovi(VmNum::U32), "R0, 0");
        storeSlot(0, 4, t4);
        std::string lTop = newLabel("run_top");
        std::string lBody = newLabel("run_body");
        std::string lEnd = newLabel("run_end");
        label(lTop);
        loadSlot(0, 4, t4);
        loadSlot(1, 4, t3);
        pushReg(0, 4);
        pushReg(1, 4);
        ins(OP_CMP_LT_U32);
        branchOnFlag(lBody, lEnd);
        label(lBody);
        // 检查组件存在（lSkip 必须在循环外定义并 label：branchOnFlag 用 operator[] 查表，
        // 未记录的 label 会解析成 0 → 跳回函数开头）
        std::string lSkip = newLabel("run_skip");
        for (const std::string& cn : sys->componentNames) {
            auto clIt = sema_->compLayout.find(cn);
            int bitsetOff = clIt != sema_->compLayout.end() ? clIt->second.bitsetOff : 0;
            loadSlot(1, 4, t4);                // e → R1（worldAddr 只破坏 R0）
            worldAddr(static_cast<uint64_t>(bitsetOff));   // [addr]
            pushReg(1, 8);                     // [addr][e]
            ins(OP_ADD_PTR);                   // [addr+e]
            popReg(2, 8);
            storeSlot(2, 8, t0);
            loadPtr(1, 1, t0);
            pushReg(1, 1);
            std::string lHas = newLabel("run_has");
            branchOnFlag(lHas, lSkip);
            label(lHas);
        }
        // 调用 update(e)（e 在 SCOPE_PUSH 前取出，避免基址错位）
        loadSlot(4, 4, t4);                    // R4 = e
        ins(OP_SCOPE_PUSH, "", "run " + tmpl);
        pushReg(4, 4);                         // arg e
        int retOff = em_.pc + 31 + (em_.counting ? 0 : codeOff_[curFuncIdx_]);
        ins(kRegPush(VmNum::U64), "R12");
        ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(retOff));
        pushReg(0, 8);
        ins(OP_ADD_U64);
        ins(kRegPush(VmNum::U64), "R12");
        ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(updOff));
        pushReg(0, 8);
        ins(OP_ADD_U64);
        ins(OP_JMP_IND);
        ins(OP_STACK_PTR_MOVE, "4");
        em_.popD(4);
        ins(OP_SCOPE_POP);
        std::string lSkip2 = newLabel("run_skip2");
        jumpTo(lSkip2);
        label(lSkip);
        label(lSkip2);
        // e++
        loadSlot(0, 4, t4);
        pushReg(0, 4);
        pushConst(4, 1);
        ins(OP_ADD_U32);
        popReg(0, 4);
        storeSlot(0, 4, t4);
        jumpTo(lTop);
        label(lEnd);
        return;
    }
    pushConst(8, 0);
}


