# How to make a skin mod for Tyr
## Requirements
- Unreal Engine 5.6
- Tyr Modding Kit

## Resources
| Tank     | Internal Name       | Class Name          |
|----------|---------------------|---------------------|
| Alecto   | TyrVehicleBlink     | BP_Blink_C          |
| Fuse     | TyrVehicleVanguard  | BP_Vanguard_Proxy_C |
| Ikarus   | TyrVehicleVtol      | BP_Vtol_Proxy_C     |
| Kestrel  | TyrVehicleDrone     | BP_Healer_C         |
| Phantom  | TyrVehicleStealth   | BP_Stealth_C        |
| Seeker   | TyrVehicleSeeker    | BP_Seeker_Proxy_C   |
| Arbalest | TyrVehicleCanOpener | BP_CanOpener_C      |
| Ark      | TyrVehicleBush      | BP_Bush_C           |
| Deadeye  | TyrVehicleDeadeye   | BP_Deadeye_C        |
| Ranger   | TyrVehicleRanger    | BP_Ranger_Proxy_C   |
| Rook     | TyrVehicleWall      | BP_Wall_Proxy_C     |
| Tempest  | TyrVehicleRecall    | BP_Recall_C         |
| Valor    | TyrVehicleHealer    | BP_Healer_C         |
| Atlas    | TyrVehicleSonar     | BP_Sonar_Proxy_C    |
| Fortis   | TyrVehicleBrawler   | BP_Brawler_Proxy_C  |
| Helio    | TyrVehicleSlowZone  | BP_SlowZone_C       |
| Maul     | TyrVehicleRam       | BP_Ram_C            |
| Tricera  | TyrVehicleSentinel  | BP_SentinelTank_C   |


---
### Step 1 - Plugin
Create a plugin with the same title as the internal name of the tank you are creating the skin for <br>
