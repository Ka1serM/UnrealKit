import numpy as np

VECTOR_NODE = "FunctionNodeInputVector"
NODE_HEADER = 36.0
NODE_ROW = 22.0
ROW_GAP = 20.0
COLUMN_GAP = 60.0
ESTIMATED_BODY_HEIGHT = {
    "ShaderNodeValue": NODE_ROW, "ShaderNodeRGB": 200.0, VECTOR_NODE: 3 * NODE_ROW, "ShaderNodeTexImage": 180.0,
    "ShaderNodeMath": 2 * NODE_ROW, "ShaderNodeVectorMath": NODE_ROW, "ShaderNodeClamp": NODE_ROW,
    "ShaderNodeMix": 2 * NODE_ROW, "ShaderNodeMapRange": 3 * NODE_ROW, "ShaderNodeUVMap": 2 * NODE_ROW,
    "ShaderNodeAttribute": 2 * NODE_ROW, "ShaderNodeVertexColor": NODE_ROW, "ShaderNodeTexWhiteNoise": NODE_ROW,
    "ShaderNodeVectorTransform": 3 * NODE_ROW, "ShaderNodeBsdfPrincipled": 2 * NODE_ROW, "ShaderNodeGroup": NODE_ROW,
}


def layout(tree, inputs, output):
    nodes = list(tree.nodes)
    index = {node.as_pointer(): i for i, node in enumerate(nodes)}
    for node in nodes:
        for socket in node.outputs:
            if not socket.is_linked:
                socket.hide = True
    offsets = {}
    heights = [estimated_height(node, offsets) for node in nodes]
    readers = [[] for _ in nodes]
    feeds = [[] for _ in nodes]
    for link in tree.links:
        source, reader = index[link.from_node.as_pointer()], index[link.to_node.as_pointer()]
        readers[source].append(reader)
        feeds[source].append((reader, offsets[link.to_socket.as_pointer()] - offsets[link.from_socket.as_pointer()]))
    input_indices = [index[node.as_pointer()] for node in inputs]
    columns = node_columns(readers, input_indices, index[output.as_pointer()])
    by_column = {}
    for i in range(len(nodes)):
        by_column.setdefault(columns[i], []).append(i)
    order = {i: rank for rank, i in enumerate(input_indices)}
    tops = [0.0] * len(nodes)
    left = 0.0
    locations = np.empty((len(nodes), 2), np.float32)
    for column in sorted(by_column):
        members = by_column[column]
        left -= max(nodes[i].width for i in members) + (COLUMN_GAP if column else 0.0)
        desired = {}
        for i in members:
            targets = [tops[reader] + offset for reader, offset in feeds[i]]
            desired[i] = sum(targets) / len(targets) if targets else 0.0
        members.sort(key=lambda i: (order.get(i, len(order)), desired[i]))
        for i, top in zip(members, pack([heights[i] for i in members], [desired[i] for i in members])):
            tops[i] = top
            locations[i] = (left, -top)
    tree.nodes.foreach_set("location", locations.ravel())


def estimated_height(node, offsets):
    y = NODE_HEADER
    for socket in node.outputs:
        if socket.enabled and not socket.hide:
            offsets[socket.as_pointer()] = y + NODE_ROW / 2
            y += NODE_ROW
    y += ESTIMATED_BODY_HEIGHT.get(node.bl_idname, 0.0)
    for socket in node.inputs:
        if not socket.enabled or socket.hide:
            continue
        offsets[socket.as_pointer()] = y + NODE_ROW / 2
        y += NODE_ROW * (4 if socket.type == "VECTOR" and not socket.is_linked and not socket.hide_value else 1)
    return y


def node_columns(readers, inputs, output):
    feeders = [[] for _ in readers]
    for source, node_readers in enumerate(readers):
        for reader in node_readers:
            feeders[reader].append(source)
    between = [False] * len(readers)
    pending = [output]
    excluded = set(inputs)
    while pending:
        i = pending.pop()
        if between[i] or i in excluded:
            continue
        between[i] = True
        pending.extend(feeders[i])
    input_column = 1 + max(longest_paths(readers, feeders, between, {}))
    return longest_paths(readers, feeders, [True] * len(readers), dict.fromkeys(inputs, input_column))


def longest_paths(readers, feeders, member, floors):
    columns = [0] * len(readers)
    waiting = [sum(member[reader] for reader in node_readers) for node_readers in readers]
    ready = [i for i, count in enumerate(waiting) if member[i] and count == 0]
    while ready:
        reader = ready.pop()
        columns[reader] = max(columns[reader], floors.get(reader, 0))
        for source in feeders[reader]:
            if not member[source]:
                continue
            columns[source] = max(columns[source], columns[reader] + 1)
            waiting[source] -= 1
            if waiting[source] == 0:
                ready.append(source)
    return columns


def pack(heights, desired):
    blocks = []
    for height, want in zip(heights, desired):
        count, size, total = 1, height + ROW_GAP, want
        while blocks and blocks[-1][3] + blocks[-1][1] > total / count:
            above_count, above_size, above_total, _ = blocks.pop()
            total = above_total + total - count * above_size
            count += above_count
            size += above_size
        blocks.append((count, size, total, total / count))
    tops = []
    for count, size, total, top in blocks:
        start = len(tops)
        for height in heights[start:start + count]:
            tops.append(top)
            top += height + ROW_GAP
    return tops
