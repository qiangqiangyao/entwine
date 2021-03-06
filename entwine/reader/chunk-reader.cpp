/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/reader/chunk-reader.hpp>

#include <pdal/PointRef.hpp>

#include <entwine/tree/chunk.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/types/binary-point-table.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/util/compression.hpp>

namespace entwine
{

ChunkReader::ChunkReader(
        const Metadata& metadata,
        const Id& id,
        const std::size_t depth,
        std::unique_ptr<std::vector<char>> data)
    : m_schema(metadata.schema())
    , m_bounds(metadata.boundsScaledCubic())
    , m_id(id)
    , m_depth(depth)
{
    Unpacker unpacker(metadata.format().unpack(std::move(data)));
    m_data = unpacker.acquireBytes();

    const std::size_t numPoints(unpacker.numPoints());

    BinaryPointTable table(m_schema);
    pdal::PointRef pointRef(table, 0);

    const std::size_t pointSize(m_schema.pointSize());
    const char* pos(m_data->data());
    Point point;

    m_points.reserve(numPoints);

    for (std::size_t i(0); i < numPoints; ++i)
    {
        table.setPoint(pos);

        point.x = pointRef.getFieldAs<double>(pdal::Dimension::Id::X);
        point.y = pointRef.getFieldAs<double>(pdal::Dimension::Id::Y);
        point.z = pointRef.getFieldAs<double>(pdal::Dimension::Id::Z);

        m_points.emplace_back(
                point,
                pos,
                Tube::calcTick(point, m_bounds, m_depth));

        pos += pointSize;
    }

    std::sort(m_points.begin(), m_points.end());
}

ChunkReader::QueryRange ChunkReader::candidates(const Bounds& queryBounds) const
{
    const PointInfo min(Tube::calcTick(queryBounds.min(), m_bounds, m_depth));
    const PointInfo max(Tube::calcTick(queryBounds.max(), m_bounds, m_depth));

    It begin(std::lower_bound(m_points.begin(), m_points.end(), min));
    It end(std::upper_bound(m_points.begin(), m_points.end(), max));

    return QueryRange(begin, end);
}

BaseChunkReader::BaseChunkReader(
        const Metadata& metadata,
        const Schema& celledSchema,
        const Id& id,
        std::unique_ptr<std::vector<char>> data)
    : m_id(id)
    , m_data()
    , m_tubes(metadata.structure().baseIndexSpan())
{
    Unpacker unpacker(metadata.format().unpack(std::move(data)));
    m_data = unpacker.acquireRawBytes();

    const std::size_t numPoints(unpacker.numPoints());

    if (metadata.format().compress())
    {
        m_data = Compression::decompress(*m_data, celledSchema, numPoints);
    }

    BinaryPointTable table(celledSchema);
    pdal::PointRef pointRef(table, 0);

    const std::size_t pointSize(celledSchema.pointSize());
    const char* pos(m_data->data());

    uint64_t tube(0);
    Point point;
    const auto tubeId(celledSchema.getId("TubeId"));
    const std::size_t dataOffset(sizeof(uint64_t));

    for (std::size_t i(0); i < numPoints; ++i)
    {
        table.setPoint(pos);

        tube = pointRef.getFieldAs<uint64_t>(tubeId);

        point.x = pointRef.getFieldAs<double>(pdal::Dimension::Id::X);
        point.y = pointRef.getFieldAs<double>(pdal::Dimension::Id::Y);
        point.z = pointRef.getFieldAs<double>(pdal::Dimension::Id::Z);

        m_tubes.at(tube).emplace_back(point, pos + dataOffset);

        pos += pointSize;
    }
}

} // namespace entwine

