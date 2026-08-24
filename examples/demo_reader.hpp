#pragma once

#include <string_view>

#include "wasafe/wasafe.hpp"

namespace Demo {

/// Синтетический источник диаграммы для примеров. Ведёт себя как настоящий
/// парсер формата — тот же двухфазный протокол Builder, — но данные генерирует
/// сам. Чтение VCD/FST в эту библиотеку не входит: такие парсеры реализуют
/// WaSafe::Reader в отдельных проектах (см. docs/writing_a_reader.md), и
/// примеры показывают ровно тот же путь подключения.
///
///   top.clk          scalar,  0@0 1@10 0@20 1@30
///   top.data         [7:0],   00000000@0 10100101@20
///   top.req          packed struct { logic [7:0] addr; logic valid; }
///   top.sub.rst      scalar,  0@0 1@20
class DemoReader final : public WaSafe::Reader {
public:
    DemoReader() = default;
    DemoReader(const DemoReader&) = delete;
    DemoReader(DemoReader&&) = delete;
    ~DemoReader() override = default;

    [[nodiscard]] std::string_view format() const override { return "demo"; }

    void read(WaSafe::Builder& sink) override {
        readHeader(sink);
        emitValues(sink);
    }

    void readHeader(WaSafe::Builder& sink) override {
        using namespace WaSafe;

        sink.setTimeScale({.exponent = static_cast<int>(TimeUnit::NS), .scale = 1});
        sink.beginScope("top", ScopeKind::MODULE);

        clk_ = sink.declareVar("clk", makeScalar());
        data_ = sink.declareVar("data", makeVector(7, 0));

        // Упакованная структура объявляется ОДНИМ declare_var: наружу она видна
        // как композит, внутри — один поток, члены которого суть битовые срезы.
        req_ = sink.declareVar("req",
                makeStruct(
                        {
                                StructMember{"addr", makeVector(7, 0), /*bitOffset*/ 1},
                                StructMember{"valid", makeScalar(), /*bitOffset*/ 0},
                        },
                        /*packed=*/true));

        sink.beginScope("sub", ScopeKind::MODULE);
        rst_ = sink.declareVar("rst", makeScalar());
        sink.endScope();

        sink.endScope();
        sink.headerDone();
    }

    DemoReader& operator=(const DemoReader&) = delete;
    DemoReader& operator=(DemoReader&&) = delete;

private:
    static WaSafe::LogicVector bits(std::uint32_t width, std::string_view chars) {
        WaSafe::LogicVector v{width};
        v.assignFromChars(chars);
        return v;
    }

    void emitValues(WaSafe::Builder& sink) const {
        const WaSafe::LogicVector lo = bits(1, "0");
        const WaSafe::LogicVector hi = bits(1, "1");

        sink.setTime(0);
        sink.valueChange(clk_, lo);
        sink.valueChange(data_, bits(8, "00000000"));
        sink.valueChange(req_, bits(9, "000000000"));  // addr=0x00, valid=0
        sink.valueChange(rst_, lo);

        sink.setTime(10);
        sink.valueChange(clk_, hi);
        sink.valueChange(req_, bits(9, "000010101"));  // addr=0x0A, valid=1

        sink.setTime(20);
        sink.valueChange(clk_, lo);
        sink.valueChange(data_, bits(8, "10100101"));
        sink.valueChange(rst_, hi);

        sink.setTime(30);
        sink.valueChange(clk_, hi);
        sink.valueChange(req_, bits(9, "101101001"));  // addr=0xB4, valid=1
    }

private:
    WaSafe::SignalId clk_;
    WaSafe::SignalId data_;
    WaSafe::SignalId req_;
    WaSafe::SignalId rst_;
};

}  // namespace Demo
