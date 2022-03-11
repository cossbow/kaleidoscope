
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <memory>

using namespace std;

//

template <typename T, typename... Ts>
unique_ptr<T> make_unique(Ts &&...params)
{
    return unique_ptr<T>(new T(forward<Ts>(params)...));
}

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
public:
    virtual ~ExprAST() {}
};

// 字面值表达式
class NumberExprAST : public ExprAST
{
public:
    NumberExprAST(double val) : val_(val) {}

private:
    double val_;
};

// 变量表达式
class VariableExprAST : public ExprAST
{
public:
    VariableExprAST(const string &name) : name_(name) {}

private:
    string name_;
};

// 二元操作表达式
class BinaryExprAST : public ExprAST
{
public:
    BinaryExprAST(char op, unique_ptr<ExprAST> lhs,
                  unique_ptr<ExprAST> rhs)
        : op_(op), lhs_(move(lhs)), rhs_(move(rhs)) {}

private:
    char op_;
    unique_ptr<ExprAST> lhs_;
    unique_ptr<ExprAST> rhs_;
};

// 函数调用表达式
class CallExprAST : public ExprAST
{
public:
    CallExprAST(const string &callee,
                vector<unique_ptr<ExprAST>> args)
        : callee_(callee), args_(move(args)) {}

private:
    string callee_;
    vector<unique_ptr<ExprAST>> args_;
};

// 函数接口
class PrototypeAST
{
public:
    PrototypeAST(const string &name, vector<string> args)
        : name_(name), args_(move(args)) {}

    const string &name() const { return name_; }

private:
    string name_;
    vector<string> args_;
};

// 函数
class FunctionAST
{
public:
    FunctionAST(unique_ptr<PrototypeAST> proto,
                unique_ptr<ExprAST> body)
        : proto_(move(proto)), body_(move(body)) {}

private:
    unique_ptr<PrototypeAST> proto_;
    unique_ptr<ExprAST> body_;
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

public:
    Parser(CharStream *stream) : stream(stream) {}
    ~Parser() {}

    string identifier();
    double number();
    int currentToken();

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

int Parser::GetToken()
{
    // 忽略空白字符
    while (isspace(last_char))
    {
        last_char = stream->next();
    }
    // 识别字符串
    if (isalpha(last_char))
    {
        g_identifier_str = last_char;
        while (isalnum((last_char = stream->next())))
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
            last_char = stream->next();
        } while (isdigit(last_char) || last_char == '.');
        g_number_val = strtod(num_str.c_str(), nullptr);
        return TOKEN_NUMBER;
    }
    // 忽略注释
    if (last_char == '#')
    {
        do
        {
            last_char = stream->next();
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
    last_char = stream->next();
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
        e = parser.GetToken();
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
            parser.ParseDefinition();
            std::cout << "parsed a function definition" << std::endl;
            break;
        }
        case TOKEN_EXTERN:
        {
            parser.ParseExtern();
            std::cout << "parsed a extern" << std::endl;
            break;
        }
        default:
        {
            parser.ParseTopLevelExpr();
            std::cout << "parsed a top level expr" << std::endl;
            break;
        }
        }
    }
    return;
}


int main(int argc, char const *argv[])
{

    // testGetToken();
    // testExpr(new FileCharStream("sample-2.txt"));
    testExpr(new StringCharStream("1+2*3-4"));

    return 0;
}
