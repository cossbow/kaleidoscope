
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <memory>

using namespace std;

#include "main.h"

// 记录了LLVM的核心数据结构，比如类型和常量表，不过我们不太需要关心它的内部
LLVMContext g_llvm_context;
// 用于创建LLVM指令
IRBuilder<> g_ir_builder(g_llvm_context);
// 用于管理函数和全局变量，可以粗浅地理解为类c++的编译单元(单个cpp文件)
Module g_module("my cool jit", g_llvm_context);
// 用于记录函数的变量参数
map<string, Value *> g_named_values;

class ASTContext
{
public:
    LLVMContext llvmContext;
    IRBuilder<> irBuilder;
    Module module;
    map<string, Value *> namedValues;

public:
    Value *doubleValue(double v)
    {
        return ConstantFP::get(llvmContext, APFloat(v));
    }
    Value *namedValue(string name)
    {
        return namedValues.at(name);
    }
    void namedValue(string name, Value *value)
    {
        namedValues[name] = value;
    }
    void namedClear()
    {
        namedValues.clear();
    }
};

//

#ifndef __cpp_lib_make_unique
template <typename T, typename... Ts>
unique_ptr<T> make_unique(Ts &&...params)
{
    return unique_ptr<T>(new T(forward<Ts>(params)...));
}
#endif

//

// 定义优先级
const map<char, int> g_binop_precedence = {
    {'<', 10}, {'+', 20}, {'-', 20}, {'*', 40}};

// 如果不是以下5种情况，Lexer返回[0-255]的ASCII值，否则返回以下枚举值
enum Token
{
    TOKEN_EOF = -1,        // 文件结束标识符
    TOKEN_DEF = -2,        // 关键字def
    TOKEN_EXTERN = -3,     // 关键字extern
    TOKEN_IDENTIFIER = -4, // 名字
    TOKEN_NUMBER = -5      // 数值
};

//

// 所有 `表达式` 节点的基类
class ExprAST
{

protected:
    ASTContext &context;

public:
    ExprAST(ASTContext &context) : context(context) {}

    virtual ~ExprAST() {}
    virtual Value *CodeGen() = 0;
};

// 字面值表达式
class NumberExprAST : public ExprAST
{
private:
    double val_;

public:
    NumberExprAST(ASTContext &context, double val) : ExprAST(context), val_(val) {}
    Value *CodeGen() override
    {
        return context.doubleValue(val_);
    }
};

// 变量表达式
class VariableExprAST : public ExprAST
{
private:
    string name_;

public:
    VariableExprAST(ASTContext &context, const string &name) : ExprAST(context), name_(name) {}
    Value *CodeGen() override
    {
        return context.namedValue(name_);
    }
};

// 二元操作表达式
class BinaryExprAST : public ExprAST
{
private:
    char op_;
    unique_ptr<ExprAST> lhs_;
    unique_ptr<ExprAST> rhs_;

public:
    BinaryExprAST(ASTContext &context, char op, unique_ptr<ExprAST> lhs,
                  unique_ptr<ExprAST> rhs)
        : ExprAST(context), op_(op), lhs_(move(lhs)), rhs_(move(rhs)) {}

    Value *CodeGen() override
    {
        Value *lhs = lhs_->CodeGen();
        Value *rhs = rhs_->CodeGen();
        switch (op_)
        {
        case '<':
        {
            Value *tmp = context.irBuilder.CreateFCmpULT(lhs, rhs, "cmptmp");
            // 把 0/1 转为 0.0/1.0
            return context.irBuilder.CreateUIToFP(
                tmp, Type::getDoubleTy(context.llvmContext), "booltmp");
        }
        case '+':
            return context.irBuilder.CreateFAdd(lhs, rhs, "addtmp");
        case '-':
            return context.irBuilder.CreateFSub(lhs, rhs, "subtmp");
        case '*':
            return context.irBuilder.CreateFMul(lhs, rhs, "multmp");
        default:
            return nullptr;
        }
    }
};

// 函数调用表达式
class CallExprAST : public ExprAST
{
private:
    string callee_;
    vector<unique_ptr<ExprAST>> args_;

public:
    CallExprAST(ASTContext &context,
                const string &callee,
                vector<unique_ptr<ExprAST>> args)
        : ExprAST(context), callee_(callee), args_(move(args)) {}
    Value *CodeGen() override
    {
        Function *callee = context.module.getFunction(callee_);
        vector<Value *> args;
        for (unique_ptr<ExprAST> &argExpr : args_)
        {
            args.push_back(argExpr->CodeGen());
        }
        return context.irBuilder.CreateCall(callee, args, "calltmp");
    }
};

// 函数接口
class PrototypeAST
{
private:
    ASTContext &context;
    string name_;
    vector<string> args_;

public:
    PrototypeAST(ASTContext &context, const string &name, vector<string> args)
        : context(context), name_(name), args_(move(args)) {}
    const string &name() const { return name_; }

    Function *CodeGen()
    {
        // 创建kaleidoscope的函数类型 double (doube, double, ..., double)
        vector<Type *> doubles(args_.size(), Type::getDoubleTy(g_llvm_context));
        // 函数类型是唯一的，所以使用get而不是new/create
        FunctionType *function_type = FunctionType::get(Type::getDoubleTy(g_llvm_context), doubles, false);
        // 创建函数, ExternalLinkage意味着函数可能不在当前module中定义，在当前module
        // 即g_module中注册名字为name_, 后面可以使用这个名字在g_module中查询
        Function *func = Function::Create(
            function_type, Function::ExternalLinkage, name_, &g_module);
        // 增加IR可读性，设置function的argument name
        int index = 0;
        for (auto &arg : func->args())
        {
            arg.setName(args_[index++]);
        }
        return func;
    }
};

// 函数
class FunctionAST
{
private:
    ASTContext &context;
    unique_ptr<PrototypeAST> proto_;
    unique_ptr<ExprAST> body_;

public:
    FunctionAST(ASTContext &context,
                unique_ptr<PrototypeAST> proto,
                unique_ptr<ExprAST> body)
        : context(context), proto_(move(proto)), body_(move(body)) {}

    Value *CodeGen()
    {
        // 检查函数声明是否已完成codegen(比如之前的extern声明), 如果没有则执行codegen
        Function *func = context.module.getFunction(proto_->name());
        if (func == nullptr)
        {
            func = proto_->CodeGen();
        }
        // 创建一个Block并且设置为指令插入位置。
        // llvm block用于定义control flow graph, 由于我们暂不实现control flow, 创建
        // 一个单独的block即可
        BasicBlock *block = BasicBlock::Create(g_llvm_context, "entry", func);
        context.irBuilder.SetInsertPoint(block);
        // 将函数参数注册到context.namedValues中，让VariableExprAST可以codegen
        context.namedClear();
        for (Value &arg : func->args())
        {
            context.namedValue(arg.getName().str(), &arg);
        }
        // codegen body然后return
        Value *ret_val = body_->CodeGen();
        context.irBuilder.CreateRet(ret_val);
        verifyFunction(*func);
        return func;
    }
};

//

class Parser
{
public:
    class CharStream
    {
    public:
        virtual int next() = 0;
    };

private:
    string g_identifier_str; // Filled in if TOKEN_IDENTIFIER
    double g_number_val;     // Filled in if TOKEN_NUMBER
    int g_current_token;

    int last_char = ' ';

    CharStream *stream;

private:
public:
    Parser(CharStream *stream) : stream(stream) {}
    ~Parser() {}

    string identifier();
    double number();
    int currentToken();

    int nextChar();
    int GetToken();
    int GetNextToken();
    int GetTokenPrecedence();

    unique_ptr<ExprAST> ParseNumberExpr();
    unique_ptr<ExprAST> ParseParenExpr();
    unique_ptr<ExprAST> ParseIdentifierExpr();
    unique_ptr<ExprAST> ParsePrimary();
    unique_ptr<ExprAST> ParseBinOpRhs(
        int min_precedence,
        unique_ptr<ExprAST> lhs);
    unique_ptr<ExprAST> ParseExpression();

    unique_ptr<PrototypeAST> ParsePrototype();
    unique_ptr<FunctionAST> ParseDefinition();
    unique_ptr<PrototypeAST> ParseExtern();
    unique_ptr<FunctionAST> ParseTopLevelExpr();
};

string Parser::identifier()
{
    return move(g_identifier_str);
}

double Parser::number()
{
    return g_number_val;
}

int Parser::nextChar()
{
    return last_char = stream->next();
}

int Parser::GetToken()
{
    // 忽略空白字符
    while (isspace(last_char))
    {
        nextChar();
    }
    // 识别字符串
    if (isalpha(last_char))
    {
        g_identifier_str = last_char;
        while (isalnum((nextChar())))
        {
            g_identifier_str += last_char;
        }
        if (g_identifier_str == "def")
        {
            return TOKEN_DEF;
        }
        else if (g_identifier_str == "extern")
        {
            return TOKEN_EXTERN;
        }
        else
        {
            return TOKEN_IDENTIFIER;
        }
    }
    // 识别数值
    if (isdigit(last_char) || last_char == '.')
    {
        string num_str;
        do
        {
            num_str += last_char;
            nextChar();
        } while (isdigit(last_char) || last_char == '.');
        g_number_val = strtod(num_str.c_str(), nullptr);
        return TOKEN_NUMBER;
    }
    // 忽略注释
    if (last_char == '#')
    {
        do
        {
            nextChar();
        } while (last_char != EOF && last_char != '\n' && last_char != '\r');
        if (last_char != EOF)
        {
            return GetToken();
        }
    }
    // 识别文件结束
    if (last_char == EOF)
    {
        return TOKEN_EOF;
    }
    // 直接返回ASCII
    int this_char = last_char;
    nextChar();
    return this_char;
}

int Parser::GetNextToken()
{
    return g_current_token = GetToken();
}
int Parser::currentToken()
{
    return g_current_token;
}

int Parser::GetTokenPrecedence()
{
    auto it = g_binop_precedence.find(g_current_token);
    if (it != g_binop_precedence.end())
    {
        return it->second;
    }
    else
    {
        return -1;
    }
}

unique_ptr<ExprAST> Parser::ParseNumberExpr()
{
    auto result = make_unique<NumberExprAST>(g_number_val);
    GetNextToken();
    return move(result);
}

// parenexpr ::= ( expression )
unique_ptr<ExprAST> Parser::ParseParenExpr()
{
    GetNextToken(); // eat (
    auto expr = ParseExpression();
    GetNextToken(); // eat )
    return expr;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier ( expression, expression, ..., expression )
unique_ptr<ExprAST> Parser::ParseIdentifierExpr()
{
    string id = g_identifier_str;
    GetNextToken();
    if (g_current_token != '(')
    {
        return make_unique<VariableExprAST>(id);
    }
    else
    {
        GetNextToken(); // eat (
        vector<unique_ptr<ExprAST>> args;
        while (g_current_token != ')')
        {
            args.push_back(ParseExpression());
            if (g_current_token == ')')
            {
                break;
            }
            else
            {
                GetNextToken(); // eat ,
            }
        }
        GetNextToken(); // eat )
        return make_unique<CallExprAST>(id, move(args));
    }
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
unique_ptr<ExprAST> Parser::ParsePrimary()
{
    switch (g_current_token)
    {
    case TOKEN_IDENTIFIER:
        return ParseIdentifierExpr();
    case TOKEN_NUMBER:
        return ParseNumberExpr();
    case '(':
        return ParseParenExpr();
    default:
        return nullptr;
    }
}

unique_ptr<ExprAST> Parser::ParseBinOpRhs(
    int min_precedence,
    unique_ptr<ExprAST> lhs)
{
    while (true)
    {
        int current_precedence = GetTokenPrecedence();
        if (current_precedence < min_precedence)
        {
            // 如果当前token不是二元操作符，current_precedence为-1, 结束任务
            // 如果遇到优先级更低的操作符，也结束任务
            return lhs;
        }
        int binop = g_current_token;
        cout << "binop: " << (char)binop << endl;
        GetNextToken(); // eat binop
        auto rhs = ParsePrimary();
        // 现在我们有两种可能的解析方式
        //    * (lhs binop rhs) binop unparsed
        //    * lhs binop (rhs binop unparsed)
        int next_precedence = GetTokenPrecedence();
        if (current_precedence < next_precedence)
        {
            // 将高于current_precedence的右边的操作符处理掉返回
            rhs = ParseBinOpRhs(current_precedence + 1, move(rhs));
        }
        lhs = make_unique<BinaryExprAST>(binop, move(lhs), move(rhs));
        // 继续循环
    }
}

// expression
//   ::= primary [binop primary] [binop primary] ...
unique_ptr<ExprAST> Parser::ParseExpression()
{
    auto lhs = ParsePrimary();
    return ParseBinOpRhs(0, move(lhs));
}

// prototype
//   ::= id ( id id ... id)
unique_ptr<PrototypeAST> Parser::ParsePrototype()
{
    string function_name = g_identifier_str;
    GetNextToken();
    vector<string> arg_names;
    while (GetNextToken() == TOKEN_IDENTIFIER)
    {
        arg_names.push_back(g_identifier_str);
    }
    GetNextToken(); // eat )
    return make_unique<PrototypeAST>(function_name, move(arg_names));
}

// definition ::= def prototype expression
unique_ptr<FunctionAST> Parser::ParseDefinition()
{
    GetNextToken(); // eat def
    auto proto = ParsePrototype();
    auto expr = ParseExpression();
    return make_unique<FunctionAST>(move(proto), move(expr));
}

// external ::= extern prototype
unique_ptr<PrototypeAST> Parser::ParseExtern()
{
    GetNextToken(); // eat extern
    return ParsePrototype();
}

// toplevelexpr ::= expression
unique_ptr<FunctionAST> Parser::ParseTopLevelExpr()
{
    auto expr = ParseExpression();
    auto proto = make_unique<PrototypeAST>("", vector<string>());
    return make_unique<FunctionAST>(move(proto), move(expr));
}

//

//
//
// 文件字符流
class FileCharStream : public Parser::CharStream
{
private:
    FILE *fp;

public:
    FileCharStream(const char *path);
    ~FileCharStream();

    int next();
};

FileCharStream::FileCharStream(const char *path)
{
    fp = fopen(path, "r");
}

FileCharStream::~FileCharStream()
{
    fclose(fp);
}

int FileCharStream::next()
{
    return fgetc(fp);
}

class StringCharStream : public Parser::CharStream
{
private:
    string source;
    int index = 0;

public:
    StringCharStream(string source) : source(source){};
    ~StringCharStream(){};

    int next();
};

int StringCharStream::next()
{
    return index < source.length() ? source.at(index++) : -1;
}

//

void testGetToken()
{
    cout << "===============================" << endl;
    FileCharStream stream("sample-1.txt");
    Parser parser(&stream);
    int e;
    while (true)
    {
        e = parser.GetNextToken();
        if (e == TOKEN_DEF)
        {
            printf("定义函数: \n");
        }
        else if (e == TOKEN_EXTERN)
        {
            printf("导出函数: \n");
        }
        else if (e == TOKEN_IDENTIFIER)
        {
            printf("identifier: %s\n", parser.identifier().c_str());
        }
        else if (e == TOKEN_NUMBER)
        {
            printf("number: %.1f\n", parser.number());
        }
        else if (e == TOKEN_EOF)
        {
            printf("结束\n");
            break;
        }
    }
}

void testExpr(Parser::CharStream *stream)
{
    cout << "===============================" << endl;
    Parser parser(stream);
    parser.GetNextToken();
    while (true)
    {
        switch (parser.currentToken())
        {
        case TOKEN_EOF:
            return;
        case TOKEN_DEF:
        {
            auto ast = parser.ParseDefinition();
            cout << "parsed a function definition" << endl;
            ast->CodeGen()->print(llvm::errs());
            std::cerr << std::endl;
            break;
        }
        case TOKEN_EXTERN:
        {
            auto ast = parser.ParseExtern();
            cout << "parsed a extern" << endl;
            ast->CodeGen()->print(llvm::errs());
            std::cerr << std::endl;
            break;
        }
        default:
        {
            auto ast = parser.ParseTopLevelExpr();
            cout << "parsed a top level expr" << endl;
            ast->CodeGen()->print(llvm::errs());
            std::cerr << std::endl;
            break;
        }
        }
    }
    return;
}

int main(int argc, char const *argv[])
{

    // testGetToken();
    testExpr(new FileCharStream("sample-2.txt"));
    // testExpr(new StringCharStream("1+2*3-4"));

    return 0;
}
