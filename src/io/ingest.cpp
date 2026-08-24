#include "wasafe/io/ingest.hpp"

namespace WaSafe {

Database ingest(Reader& reader, Builder& sink) {
    reader.read(sink);
    sink.finish();
    return sink.takeDatabase();
}

Database ingestHeader(Reader& reader, Builder& sink) {
    reader.readHeader(sink);
    sink.finish();
    return sink.takeDatabase();
}

}  // namespace WaSafe
