myid = 99999
hello_targets = {}

function set_uid(x)
    myid = x
end

function event_player_move(player)
    local player_x = API_get_x(player)
    local player_y = API_get_y(player)
    local my_x = API_get_x(myid)
    local my_y = API_get_y(myid)

    if player_x == my_x and player_y == my_y then
        if hello_targets[player] == nil then
            API_SendMessage(myid, player, "HELLO")
            hello_targets[player] = { move_count = 0 }
        end
    end
end

function event_npc_move()
    for player, data in pairs(hello_targets) do
        data.move_count = data.move_count + 1
        if data.move_count >= 3 then
            API_SendMessage(myid, player, "BYE")
            hello_targets[player] = nil
        end
    end
end